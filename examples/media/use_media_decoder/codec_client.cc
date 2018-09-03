// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "codec_client.h"

#include "util.h"

namespace {

// The client would like there to be at least this many extra input packets in
// addition to the bare minimum required for the codec to be able to function.
// Using 1 or 2 here can help avoid short stalls while packets are in transit
// even if the client doesn't actually need to hold any free input packets for
// any significant duration for any client-specific reason.
constexpr uint32_t kMinExtraInputPacketsForClient = 2;
// The client would like there to be at least this many extra output packets in
// addition to the bare minimum required for the codec to be able to function.
// The client _must_ use a non-zero value here if any output packets will be
// held indefinitely (such as held until the next output packet is available),
// since otherwise the Codec can be unable to continue processing.
constexpr uint32_t kMinExtraOutputPacketsForClient = 2;

// For input, this example doesn't re-configure input buffers, so there's only
// one buffer_lifetime_ordinal.
constexpr uint64_t kInputBufferLifetimeOrdinal = 1;

}  // namespace

CodecClient::CodecClient(async::Loop* loop)
    : loop_(loop), dispatcher_(loop_->dispatcher()) {
  // We haven't created a channel yet, but that's fine, and we want the error
  // handler set up before any error can possibly be generated by the channel so
  // there's no chance of missing an error.  The async::Loop that we'll use is
  // already running separately from the current thread.
  codec_.set_error_handler([] {
    // Obviously a non-example client that continues to have a purpose even if
    // one of its codecs dies would want to handle errors in a more contained
    // way.
    //
    // TODO(dustingreen): get and print epitaph once that's possible.
    Exit("codec_ failed - exiting\n");
  });

  // Only one request is ever created, so we create it in the constructor to
  // avoid needing any manual enforcement that we only do this once.
  temp_codec_request_ = codec_.NewRequest(loop_->dispatcher());

  // We treat event setup as much as possible like a hidden part of creating the
  // CodecPtr.  If NewBinding() has !is_valid(), we rely on the Codec server to
  // close the Codec channel async.
  codec_.events().OnStreamFailed =
      fit::bind_member(this, &CodecClient::OnStreamFailed);
  codec_.events().OnInputConstraints =
      fit::bind_member(this, &CodecClient::OnInputConstraints);
  codec_.events().OnFreeInputPacket =
      fit::bind_member(this, &CodecClient::OnFreeInputPacket);
  codec_.events().OnOutputConfig =
      fit::bind_member(this, &CodecClient::OnOutputConfig);
  codec_.events().OnOutputPacket =
      fit::bind_member(this, &CodecClient::OnOutputPacket);
  codec_.events().OnOutputEndOfStream =
      fit::bind_member(this, &CodecClient::OnOutputEndOfStream);
}

CodecClient::~CodecClient() { Stop(); }

fidl::InterfaceRequest<fuchsia::mediacodec::Codec>
CodecClient::GetTheRequestOnce() {
  return std::move(temp_codec_request_);
}

void CodecClient::Start() {
  // The caller is responsible for calling this method only once, using the main
  // thread.  This method only holds the lock for short periods, and has to
  // release the lock many times during this method, which is reasonable given
  // the nature of this method as an overall state progression sequencer.

  // Call Sync() and wait for it's response _only_ to force the Codec server to
  // reach the point of being able response to messages, just for easier
  // debugging if just starting the Codec server fails instead.  Actual clients
  // don't need to use Sync() here.
  CallSyncAndWaitForResponse();
  VLOGF("Sync() completed, which means the Codec server exists.\n");

  VLOGF("Waiting for OnInputConstraints() from the Codec server...\n");
  // The Codec client can rely on an OnInputConstraints() arriving shortly,
  // without any message required from the client first.  The
  // OnInputConstraints() may in future actually be sent by the CodecFactory,
  // but it'll still be sent to the client on the Codec channel in any case.
  {  // scope lock
    std::unique_lock<std::mutex> lock(lock_);
    // In this example we're not paying attention to channel failure here
    // because channel failure calls exit().
    while (!input_constraints_) {
      input_constraints_exist_condition_.wait(lock);
    }
  }  // ~lock
  assert(input_constraints_);
  VLOGF("Got OnInputConstraints() from the Codec server.\n");

  // We know input_constraints_ won't change outside the lock because we prevent
  // that in OnInputConstraints() by only accepting input constraints if there
  // aren't already input constraints.

  // Now that we have input constraints, we can create all the input buffers and
  // tell the Codec server about them.  We tell the Codec server by using
  // SetInputSettings() followed by one or num_buffers calls to
  // AddInputBuffer().  These are necessary before it becomes permissible to
  // call CreateStream().
  //
  // We're not on the FIDL thread, so we need to PostSerial() over to the FIDL
  // thread to send any FIDL message.

  uint32_t packet_count_for_client = kMinExtraInputPacketsForClient;
  uint32_t packet_count_for_codec =
      input_constraints_->packet_count_for_codec_recommended;
  uint32_t input_packet_count =
      packet_count_for_client + packet_count_for_codec;
  if (input_packet_count < packet_count_for_codec ||
      input_packet_count > input_constraints_->packet_count_for_codec_max) {
    Exit(
        "server can't easily accomodate kMinExtraInputPacketsForClient - not "
        "using server - exiting\n");
  }
  {  // scope input_settings, just for clarity
    fuchsia::mediacodec::CodecPortBufferSettings input_settings{
        .buffer_lifetime_ordinal = kInputBufferLifetimeOrdinal,
        .buffer_constraints_version_ordinal =
            input_constraints_->buffer_constraints_version_ordinal,
        .packet_count_for_codec = packet_count_for_codec,
        .packet_count_for_client = packet_count_for_client,
        .per_packet_buffer_bytes =
            input_constraints_->per_packet_buffer_bytes_recommended,
        .single_buffer_mode = false,
    };
    PostSerial(dispatcher_,
               [this, input_settings = std::move(input_settings)]() mutable {
                 codec_->SetInputBufferSettings(std::move(input_settings));
               });
  }
  assert(input_free_bits_.empty());
  input_free_bits_.resize(input_packet_count, true);
  all_input_buffers_.reserve(input_packet_count);
  for (uint32_t i = 0; i < input_packet_count; i++) {
    std::unique_ptr<CodecBuffer> local_buffer =
        CodecBuffer::Allocate(i, *input_constraints_);
    if (!local_buffer) {
      Exit("CodecBuffer::Allocate() failed");
    }
    zx::vmo dup_vmo;
    if (!local_buffer->GetDupVmo(false, &dup_vmo)) {
      Exit("GetDupVmo() failed");
    }
    assert(all_input_buffers_.size() == i);
    all_input_buffers_.push_back(std::move(local_buffer));

    // May as well tell the Codec server about these incrementally.
    {
      fuchsia::mediacodec::CodecBuffer codec_buffer{
          .buffer_lifetime_ordinal = kInputBufferLifetimeOrdinal,
          .buffer_index = i,
      };
      codec_buffer.data.set_vmo(fuchsia::mediacodec::CodecBufferDataVmo{
          .vmo_handle = std::move(dup_vmo),
          .vmo_usable_start = 0,
          .vmo_usable_size =
              input_constraints_->per_packet_buffer_bytes_recommended,
      });
      PostSerial(dispatcher_,
                 [this, codec_buffer = std::move(codec_buffer)]() mutable {
                   codec_->AddInputBuffer(std::move(codec_buffer));
                 });
    }
  }
  // Now that the codec has all the input buffers, we effectively have just
  // allocated all the input packets.  They all start as free with the Codec
  // client, per protocol.
  input_free_list_.reserve(input_packet_count);
  for (uint32_t i = 0; i < input_packet_count; i++) {
    input_free_list_.push_back(i);
  }
}

void CodecClient::Stop() {
  if (codec_.is_bound()) {
    codec_.Unbind();
  }
}

void CodecClient::CallSyncAndWaitForResponse() {
  // Just for clarity of the example, we'll use a local lock here, since that's
  // all we actually need.
  std::mutex is_sync_complete_lock;
  bool is_sync_complete = false;
  std::condition_variable is_sync_complete_condition;
  // Capturing stuff with just "&" is sometimes frowned upon, but in this case
  // there's no chance of any lambda outliving anything, so it's fine.  The
  // outer lambda is because ProxyController isn't thread-safe and the present
  // method is called from the main thread not the FIDL thread, so we have to
  // switch threads to send a FIDL message.  The inner lambda is the completion
  // callback.
  VLOGF("before calling Sync() (main thread)...\n");
  PostSerial(dispatcher_, [&] {
    VLOGF("before calling Sync() (fidl thread)...\n");
    codec_->Sync([&]() {
      {  // scope lock
        std::unique_lock<std::mutex> lock(is_sync_complete_lock);
        is_sync_complete = true;
      }  // ~lock
      is_sync_complete_condition.notify_all();
    });
  });
  VLOGF("after calling Sync() - waiting...\n");
  {  // scope lock
    std::unique_lock<std::mutex> lock(is_sync_complete_lock);
    // We rely on the channel error handler to be doing an exit() for this loop
    // to be reasonable without checking for channel failure here.
    while (!is_sync_complete) {
      is_sync_complete_condition.wait(lock);
    }
  }
  VLOGF("after calling Sync() - done waiting\n");
  assert(is_sync_complete);
}

void CodecClient::OnInputConstraints(
    fuchsia::mediacodec::CodecBufferConstraints input_constraints) {
  if (input_constraints_) {
    Exit(
        "server sent more than one input constraints without client asking "
        "for a re-send");
  }
  {  // scope lock
    std::unique_lock<std::mutex> lock(lock_);
    input_constraints_ =
        std::make_unique<fuchsia::mediacodec::CodecBufferConstraints>(
            std::move(input_constraints));
  }  // ~lock
  input_constraints_exist_condition_.notify_all();
}

void CodecClient::OnFreeInputPacket(
    fuchsia::mediacodec::CodecPacketHeader free_input_packet) {
  bool was_empty;
  {  // scope lock
    std::unique_lock<std::mutex> lock(lock_);
    if (input_free_bits_[free_input_packet.packet_index]) {
      // already free - a normal client wouldn't want to just exit here since
      // this is the server's fault - in this example we just care that we
      // detect it
      Exit(
          "OnFreeInputPacket() when already free - server's fault? - "
          "packet_index: %d",
          free_input_packet.packet_index);
    }
    was_empty = input_free_list_.empty();
    input_free_list_.push_back(free_input_packet.packet_index);
    input_free_bits_[free_input_packet.packet_index] = true;
  }  // ~lock
  if (was_empty) {
    input_free_list_not_empty_.notify_all();
  }
}

std::unique_ptr<fuchsia::mediacodec::CodecPacket>
CodecClient::BlockingGetFreeInputPacket() {
  uint32_t free_index;
  {  // scope lock
    std::unique_lock<std::mutex> lock(lock_);
    while (input_free_list_.empty()) {
      input_free_list_not_empty_.wait(lock);
    }
    free_index = input_free_list_.back();
    input_free_list_.pop_back();
    // We intentionally do not modify input_free_bits_ here, as those bits are
    // tracking the protocol level free-ness, so will get updated when the
    // caller queues the input packet.
    assert(input_free_bits_[free_index]);
  }
  std::unique_ptr<fuchsia::mediacodec::CodecPacket> packet =
      fuchsia::mediacodec::CodecPacket::New();
  packet->header.buffer_lifetime_ordinal = kInputBufferLifetimeOrdinal;
  packet->header.packet_index = free_index;
  return packet;
}

const CodecBuffer& CodecClient::GetInputBufferByIndex(uint32_t packet_index) {
  return *all_input_buffers_[packet_index];
}

const CodecBuffer& CodecClient::GetOutputBufferByIndex(uint32_t packet_index) {
  return *all_output_buffers_[packet_index];
}

void CodecClient::QueueInputFormatDetails(
    uint64_t stream_lifetime_ordinal,
    fuchsia::mediacodec::CodecFormatDetails input_format_details) {
  PostSerial(dispatcher_, [this, stream_lifetime_ordinal,
                           input_format_details =
                               std::move(input_format_details)]() mutable {
    codec_->QueueInputFormatDetails(stream_lifetime_ordinal,
                                    std::move(input_format_details));
  });
}

// buffer - the populated input packet buffer, or an empty input buffers with
//   end_of_stream set on it.
void CodecClient::QueueInputPacket(
    std::unique_ptr<fuchsia::mediacodec::CodecPacket> packet) {
  // Intentional copy.
  fuchsia::mediacodec::CodecPacket local_packet = *packet;
  {  // scope lock
    // This packet is already not on the free list, but is still considered free
    // from a protocol point of view, so update that part.
    std::unique_lock<std::mutex> lock(lock_);
    assert(input_free_bits_[local_packet.header.packet_index]);
    input_free_bits_[local_packet.header.packet_index] = false;
    // From here it's as if this packet is already in flight with the server.
  }  // ~lock
  PostSerial(dispatcher_, [this, packet = std::move(local_packet)] {
    codec_->QueueInputPacket(std::move(packet));
  });
}

void CodecClient::QueueInputEndOfStream(uint64_t stream_lifetime_ordinal) {
  PostSerial(dispatcher_, [this, stream_lifetime_ordinal] {
    codec_->QueueInputEndOfStream(stream_lifetime_ordinal);
  });
}

std::unique_ptr<CodecOutput> CodecClient::BlockingGetEmittedOutput() {
  while (true) {
    // The rule is that a required pending config won't be followed by any more
    // output packets until it's no longer pending (in the sense that the output
    // buffers have been suitably re-configured).  We verify the server is
    // following that rule elsewhere, which means we know here that when both
    // packets are pending and config is pending, the packets were delivered to
    // the client first.  So we drain the packets first.
    std::unique_ptr<CodecOutput> packet;
    std::shared_ptr<const fuchsia::mediacodec::CodecOutputConfig> config;
    {  // scope lock
      std::unique_lock<std::mutex> lock(lock_);
      while (!output_pending_) {
        output_pending_condition_.wait(lock);
      }
      if (!emitted_output_.empty()) {
        packet = std::move(emitted_output_.front());
        emitted_output_.pop_front();
        // This only does anything when the last packet is consumed and there is
        // no config pending.
        if (!ComputeOutputPendingLocked()) {
          output_pending_ = false;
        }
      } else {
        assert(output_config_action_pending_);
        config = last_required_output_config_;
      }
    }

    // Now we own a packet or have a required config to deal with, but not both,
    // so it doesn't matter which order we check here, but for clarity we check
    // in the same order as above.
    if (packet) {
      return packet;
    }

    // We have a required output config change to deal with here.

    // The server implicitly has relinquished ownership of all output packets
    // and all output buffers as a semantic of the required config change.  This
    // shouldn't really be thought of the packets being "emitted" - rather from
    // the server's point of view they're deallocated already.  Now it's the
    // client's turn to deallocate the old buffers.  It is not permitted to
    // re-use a previous buffer as a new buffer, per protocol rules, not even
    // for the same Codec instance, and not even for a mid-stream output config
    // change.
    //
    // The main mechanism used to detect that the server isn't sending output
    // too soon is output_config_action_pending_.  In contrast, the client code
    // in this example permits itself to send RecycleOutputPacket() after the
    // client has already seen OnOutputConfig() with action required true, even
    // though the client could stop itself from doing so as a potential
    // optimization.  The client is allowed to send RecycleOutputPacket() up
    // until the implied ReleaseOutputBuffers() at the start of
    // SetOutputSettings().  Between then and a given packet_index becoming
    // emitted again (!free), a RecycleOutputPacket() for that packet_index is
    // forbidden.
    //
    // Because of the client allowing itself to send RecycleOutputPacket() for a
    // while longer than fundamentally necessary, we delay upkeep on
    // output_free_bits_ until here.  This upkeep isn't really fundamentally
    // necessary between OnOutputConfig() with action required true and the last
    // AddOutputBuffer() as part of output re-configuration, but ... this
    // explicit delayed upkeep _may_ help illustrate how it's acceptable for a
    // client to let the completion end of output processing send
    // RecycleOutputPacket() as long as all those will be sent before
    // SetOutputSettings().
    {  // scope lock
      std::unique_lock<std::mutex> lock(lock_);

      // We know this because the previous OnOutputConfig() set this and because
      // we're only here if it's set.
      assert(output_config_action_pending_);
      // We know this because we reject additional output from the server when
      // output_config_action_pending_ is true, and because we've drained all
      // previous output by this point.
      assert(emitted_output_.empty());
      // We know this because we're only here if we have a pending config.
      assert(config);

      // Not really critical to do this, as we'll just end up setting these
      // back to true under the same lock hold interval as we set
      // output_config_action_pending_ to false, but see comment above re.
      // how this might help illustrate how late RecycleOutputPacket() can be
      // sent.
      //
      // Think of this assignment as slightly more than a comment in this
      // example, rather than any real need.
      output_free_bits_.resize(0);
    }  // ~lock

    // Free the old output buffers, if any.
    while (!all_output_buffers_.empty()) {
      std::unique_ptr<CodecBuffer> buffer =
          std::move(all_output_buffers_.back());
      all_output_buffers_.pop_back();
    }

    // Here is where we snap which exact config version we'll actually use.
    //
    // For a client that's doing output buffer re-config on the FIDL thread
    // during OnOutputConfig with action required true, this will always just be
    // the config being presently received.  But this example shows how to drive
    // the codec in a protocol-valid way without being forced to perform buffer
    // re-configuration on the FIDL thread.

    std::shared_ptr<const fuchsia::mediacodec::CodecOutputConfig>
        snapped_config;
    uint64_t new_output_buffer_lifetime_ordinal;
    {  // scope lock
      std::unique_lock<std::mutex> lock(lock_);
      // We'll snap the last_output_config_, which is always at least as recent
      // as the last_required_output_config_.
      snapped_config = last_output_config_;
      new_output_buffer_lifetime_ordinal = next_output_buffer_lifetime_ordinal_;
      next_output_buffer_lifetime_ordinal_ += 2;
    }  // ~lock

    // Tell the server about output settings.

    const fuchsia::mediacodec::CodecBufferConstraints& constraints =
        snapped_config->buffer_constraints;
    uint32_t packet_count_for_codec =
        constraints.packet_count_for_codec_recommended;
    uint32_t packet_count_for_client = kMinExtraOutputPacketsForClient;
    uint32_t packet_count = packet_count_for_codec + packet_count_for_client;
    // printf("Sending SetOutputBufferSettings - buffer_lifetime_ordinal: %lu
    // buffer_constraints_version_ordinal: %lu\n",
    // new_output_buffer_lifetime_ordinal,
    // constraints.buffer_constraints_version_ordinal);
    PostSerial(dispatcher_,
               [this, settings = fuchsia::mediacodec::CodecPortBufferSettings{
                          .buffer_lifetime_ordinal =
                              new_output_buffer_lifetime_ordinal,
                          .buffer_constraints_version_ordinal =
                              constraints.buffer_constraints_version_ordinal,
                          .packet_count_for_codec = packet_count_for_codec,
                          .packet_count_for_client = packet_count_for_client,
                          .per_packet_buffer_bytes =
                              constraints.per_packet_buffer_bytes_recommended,
                          .single_buffer_mode = false,
                      }]() mutable {
                 codec_->SetOutputBufferSettings(std::move(settings));
               });

    // Allocate new output buffers and tell the server about them.  Telling the
    // server about the last buffer is significant in the protocol.  See details
    // below.
    //
    // This example doesn't try to skip creating and configuring the rest of the
    // buffers if a new action-required config has arrived, but doing so would
    // be legal behavior per the protocol.

    all_output_buffers_.reserve(packet_count);
    for (uint32_t i = 0; i < packet_count; i++) {
      std::unique_ptr<CodecBuffer> buffer =
          CodecBuffer::Allocate(i, constraints);
      if (!buffer) {
        Exit("CodecBuffer::Allocate() failed (output)");
      }
      zx::vmo dup_vmo;
      if (!buffer->GetDupVmo(true, &dup_vmo)) {
        Exit("GetDupVmo() failed (output)");
      }
      assert(all_output_buffers_.size() == i);
      all_output_buffers_.push_back(std::move(buffer));

      // The last buffer being added is significant to the protocol.
      if (i == packet_count - 1) {
        std::unique_lock<std::mutex> lock(lock_);
        // The last mesage will potentially result in OnOutputPacket(), so we
        // need to be ready for that packet.
        //
        // This is non-harmful if output_config_action_pending_ will remain
        // true.
        output_free_bits_.resize(packet_count, true);
      }

      {  // scope codec_buffer for clarity
        fuchsia::mediacodec::CodecBuffer codec_buffer{
            .buffer_lifetime_ordinal = new_output_buffer_lifetime_ordinal,
            .buffer_index = i,
        };
        codec_buffer.data.set_vmo(fuchsia::mediacodec::CodecBufferDataVmo{
            .vmo_handle = std::move(dup_vmo),
            .vmo_usable_start = 0,
            .vmo_usable_size = constraints.per_packet_buffer_bytes_recommended,
        });
        // printf("Sending AddOutputBuffer - buffer_lifetime_ordinal: %lu\n",
        // new_output_buffer_lifetime_ordinal);
        PostSerial(dispatcher_,
                   [this, codec_buffer = std::move(codec_buffer)]() mutable {
                     codec_->AddOutputBuffer(std::move(codec_buffer));
                   });
      }
    }

    // So, now that we're done with that output re-config, it's time to see if
    // that re-config was the last one we need to do, or if there's a newer
    // config that's action-required.

    {  // scope lock
      std::unique_lock<std::mutex> lock(lock_);
      if (snapped_config->buffer_constraints
              .buffer_constraints_version_ordinal >=
          last_required_output_config_->buffer_constraints
              .buffer_constraints_version_ordinal) {
        // Good.  The client is caught up.  The output_config_action_pending_
        // can become false here, but may very shortly become true again if
        // another OnOutputConfig() is received after we release the lock
        // (roughly speaking; see code).
        //
        // It's ok that we didn't set output_config_action_pending_ to false
        // before sending the last AddOutputBuffer() above, because
        // OnOutputConfig() was still able to update
        // last_required_output_config_ as needed, which it's been able to do
        // all along during most of this whole method.  If we had set to false
        // up there, it would probably be less obvious why it works vs. here,
        // but either can work.
        VLOGF(
            "output_config_action_pending_ = false, because client caught "
            "up\n");
        output_config_action_pending_ = false;
        // Because this was true for at least pending config reason which we
        // are only just clearning immediately above.
        assert(output_pending_);
        // There can be output packets by this point so only clear
        // output_pending_ if there are also no packets.
        if (!ComputeOutputPendingLocked()) {
          output_pending_ = false;
        }
      } else {
        // We've received and even more recent config that's action-required, so
        // go around again without clearing output_config_action_pending_ or
        // output_pending_.  Both remain true until we've caught up to a config
        // that's at least as new as the last_required_output_config_.
        VLOGF(
            "output_config_action_pending_ remains true because server has "
            "sent yet another action-required output config\n");
        assert(output_config_action_pending_);
        assert(output_pending_);
      }
    }  // ~lock
  }
}

void CodecClient::RecycleOutputPacket(
    fuchsia::mediacodec::CodecPacketHeader free_packet) {
  {  // scope lock
    std::unique_lock<std::mutex> lock(lock_);
    output_free_bits_[free_packet.packet_index] = true;
  }  // ~lock
  PostSerial(dispatcher_,
             [this, free_packet] { codec_->RecycleOutputPacket(free_packet); });
}

void CodecClient::OnOutputConfig(
    fuchsia::mediacodec::CodecOutputConfig output_config) {
  bool output_pending_notify_needed = false;
  // Not that the std::move() actaully helps here, but that's what we're doing.
  std::shared_ptr<fuchsia::mediacodec::CodecOutputConfig> shared_config =
      std::make_shared<fuchsia::mediacodec::CodecOutputConfig>(
          std::move(output_config));
  {  // scope lock
    std::unique_lock<std::mutex> lock(lock_);
    uint64_t previous_buffer_constraints_version_ordinal =
        last_output_config_ ? last_output_config_->buffer_constraints
                                  .buffer_constraints_version_ordinal
                            : 0;
    if (shared_config->buffer_constraints.buffer_constraints_version_ordinal <
        previous_buffer_constraints_version_ordinal) {
      Exit("broken server sent badly ordered buffer constraints ordinals");
    }
    if (shared_config->buffer_constraints_action_required &&
        shared_config->buffer_constraints.buffer_constraints_version_ordinal <=
            previous_buffer_constraints_version_ordinal) {
      Exit(
          "broken server sent buffer_constraints_action_required without "
          "increasing buffer_constraints_version_ordinal");
    }
    last_output_config_ = shared_config;
    VLOGF(
        "OnOutputConfig buffer_constraints_version_ordinal: %lu "
        "buffer_constraints_action_required: %d\n",
        shared_config->buffer_constraints.buffer_constraints_version_ordinal,
        shared_config->buffer_constraints_action_required);
    if (shared_config->buffer_constraints_action_required) {
      last_required_output_config_ = shared_config;
      VLOGF(
          "output_config_action_pending_ = true, because received a "
          "buffer_constraints_action_required config\n");
      output_config_action_pending_ = true;
      if (!output_pending_) {
        output_pending_ = true;
        output_pending_notify_needed = true;
      }
    }
  }  // ~lock
  if (output_pending_notify_needed) {
    output_pending_condition_.notify_all();
  }
}

void CodecClient::OnOutputPacket(fuchsia::mediacodec::CodecPacket output_packet,
                                 bool error_detected_before,
                                 bool error_detected_during) {
  bool output_pending_notify_needed = false;
  std::unique_ptr<const fuchsia::mediacodec::CodecPacket> local_packet =
      std::make_unique<fuchsia::mediacodec::CodecPacket>(
          std::move(output_packet));
  uint32_t packet_index = local_packet->header.packet_index;
  // This is safe outside the lock, because last_output_config_ is only updated
  // by OnOutputConfig(), which can't happen simultaneously with
  // OnOutputPacket().
  uint64_t stream_lifetime_ordinal = local_packet->stream_lifetime_ordinal;
  std::unique_ptr<CodecOutput> output = std::make_unique<CodecOutput>(
      stream_lifetime_ordinal, last_output_config_, std::move(local_packet),
      false);
  {  // scope lock
    std::unique_lock<std::mutex> lock(lock_);
    if (output_config_action_pending_) {
      // FWIW, we wouldn't be able to detect this if we were using the
      // async::Loop thread to perform output buffer re-configuration.
      Exit(
          "server incorrectly sent output packet while required config change "
          "pending");
    }
    if (!output_free_bits_[packet_index]) {
      // The packet was emitted twice by the server without it becoming free in
      // between, which is broken server behavior.
      Exit(
          "server incorrectly emitted an output packet without it becoming "
          "free in between");
    }
    // Emitted by server, so not free until later when we send it back to server
    // with RecycleOutputPacket(), or until we re-configure output buffers in
    // which case all the output packets start free with the server.
    output_free_bits_[packet_index] = false;
    emitted_output_.push_back(std::move(output));
    if (!output_pending_) {
      output_pending_ = true;
      output_pending_notify_needed = true;
    }
  }  // ~lock
  if (output_pending_notify_needed) {
    output_pending_condition_.notify_all();
  }
}

void CodecClient::OnOutputEndOfStream(uint64_t stream_lifetime_ordinal,
                                      bool error_detected_before) {
  bool output_pending_notify_needed = false;
  std::unique_ptr<CodecOutput> output = std::make_unique<CodecOutput>(
      stream_lifetime_ordinal, nullptr, nullptr, true);
  {  // scope lock
    std::unique_lock<std::mutex> lock(lock_);
    if (output_config_action_pending_) {
      // FWIW, we wouldn't be able to detect this if we were using the
      // async::Loop thread to perform output buffer re-configuration.
      Exit(
          "server incorrectly sent OnOutputEndOfStream() while required config "
          "change pending");
    }
    emitted_output_.push_back(std::move(output));
    if (!output_pending_) {
      output_pending_ = true;
      output_pending_notify_needed = true;
    }
  }  // ~lock
  if (output_pending_notify_needed) {
    output_pending_condition_.notify_all();
  }
}

void CodecClient::OnStreamFailed(uint64_t stream_lifetime_ordinal) {
  assert(false && "not implemented");
}