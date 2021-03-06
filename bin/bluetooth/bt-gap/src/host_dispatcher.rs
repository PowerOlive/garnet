// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::host_device::{self, HostDevice};
use crate::services;
use crate::store::stash::Stash;
use crate::util;
use failure::Error;
use fidl;
use fidl::encoding::OutOfLine;
use fidl_fuchsia_bluetooth;
use fidl_fuchsia_bluetooth_control::{
    AdapterInfo,
    ControlControlHandle,
    PairingDelegateMarker,
    PairingDelegateProxy,
};
use fidl_fuchsia_bluetooth_control::{InputCapabilityType, OutputCapabilityType};
use fidl_fuchsia_bluetooth_host::HostProxy;
use fidl_fuchsia_bluetooth_le::CentralProxy;
use fuchsia_async::{
    self as fasync,
    temp::Either::{Left, Right},
    TimeoutExt,
};
use fuchsia_bluetooth::{
    self as bt, bt_fidl_status, error::Error as BTError, util::clone_host_info,
};
use fuchsia_syslog::{fx_log, fx_log_err, fx_log_info, fx_log_warn};
use fuchsia_vfs_watcher as vfs_watcher;
use fuchsia_zircon as zx;
use fuchsia_zircon::Duration;
use futures::future;
use futures::TryStreamExt;
use futures::{task::{LocalWaker, Waker}, Future, Poll, TryFutureExt};
use parking_lot::RwLock;
use slab::Slab;
use std::collections::HashMap;
use std::fs::File;
use std::io;
use std::marker::Unpin;
use std::path::PathBuf;
use std::sync::{Arc, Weak};

pub static HOST_INIT_TIMEOUT: i64 = 5; // Seconds

static BT_HOST_DIR: &'static str = "/dev/class/bt-host";
static DEFAULT_NAME: &'static str = "fuchsia";

pub struct DiscoveryRequestToken {
    adap: Weak<RwLock<HostDevice>>,
}

impl Drop for DiscoveryRequestToken {
    fn drop(&mut self) {
        if let Some(host) = self.adap.upgrade() {
            let host = host.write();
            host.stop_discovery();
        }
    }
}

pub struct DiscoverableRequestToken {
    adap: Weak<RwLock<HostDevice>>,
}

impl Drop for DiscoverableRequestToken {
    fn drop(&mut self) {
        if let Some(host) = self.adap.upgrade() {
            let mut host = host.write();
            host.set_discoverable(false);
        }
    }
}

pub struct HostDispatcher {
    host_devices: HashMap<String, Arc<RwLock<HostDevice>>>,
    active_id: Option<String>,

    // Component storage.
    pub stash: Stash,

    // GAP state
    name: String,
    discovery: Option<Weak<DiscoveryRequestToken>>,
    discoverable: Option<Weak<DiscoverableRequestToken>>,
    pub input: InputCapabilityType,
    pub output: OutputCapabilityType,

    pub pairing_delegate: Option<PairingDelegateProxy>,
    pub event_listeners: Vec<ControlControlHandle>,

    // Pending requests to obtain a Host.
    host_requests: Slab<Waker>,
}

impl HostDispatcher {
    pub fn new(stash: Stash) -> HostDispatcher {
        HostDispatcher {
            active_id: None,
            stash: stash,
            host_devices: HashMap::new(),
            name: DEFAULT_NAME.to_string(),
            input: InputCapabilityType::None,
            output: OutputCapabilityType::None,
            discovery: None,
            discoverable: None,
            pairing_delegate: None,
            event_listeners: vec![],
            host_requests: Slab::new(),
        }
    }

    /// Used to set the pairing delegate. If there is a prior pairing delegate connected to the
    /// host it will fail. It checks if the existing stored connection is closed, and will
    /// overwrite it if so.
    pub fn set_pairing_delegate(&mut self, delegate: Option<PairingDelegateProxy>) -> bool {
        match delegate {
            Some(delegate) => {
                let assign = match self.pairing_delegate {
                    None => true,
                    Some(ref pd) => pd.is_closed(),
                };
                if assign {
                    self.pairing_delegate = Some(delegate);
                }
                assign
            }
            None => {
                self.pairing_delegate = None;
                false
            }
        }
    }

    /// Returns the current pairing delegate proxy if it exists and has not been closed. Clears the
    /// if the handle is closed.
    pub fn pairing_delegate(&mut self) -> Option<PairingDelegateProxy> {
        if let Some(delegate) = &self.pairing_delegate {
            if delegate.is_closed() {
                self.pairing_delegate = None;
            }
        }
        self.pairing_delegate.clone()
    }

    pub fn set_name(
        hd: Arc<RwLock<HostDispatcher>>,
        name: Option<String>,
    ) -> impl Future<Output = fidl::Result<fidl_fuchsia_bluetooth::Status>> {
        hd.write().name = match name {
            Some(name) => name,
            None => DEFAULT_NAME.to_string(),
        };
        HostDispatcher::get_active_adapter(hd.clone()).and_then(move |adapter| match adapter {
            Some(adapter) => Left(adapter.write().set_name(hd.read().name.clone())),
            None => Right(future::ready(Ok(bt_fidl_status!(
                BluetoothNotAvailable,
                "No Adapter found"
            )))),
        })
    }

    /// Return the active id. If the ID is current not set,
    /// this fn will make the first ID in it's host_devices active
    fn get_active_id(&mut self) -> Option<String> {
        match self.active_id {
            None => {
                let id = match self.host_devices.keys().next() {
                    None => {
                        return None;
                    }
                    Some(id) => id.clone(),
                };
                self.set_active_id(Some(id));
                self.active_id.clone()
            }
            ref id => id.clone(),
        }
    }

    pub fn start_discovery(
        hd: Arc<RwLock<HostDispatcher>>,
    ) -> impl Future<
        Output = fidl::Result<(
            fidl_fuchsia_bluetooth::Status,
            Option<Arc<DiscoveryRequestToken>>,
        )>,
    > {
        let strong_current_token = match hd.read().discovery {
            Some(ref token) => token.upgrade(),
            None => None,
        };
        if let Some(token) = strong_current_token {
            return Left(future::ready(Ok((
                bt_fidl_status!(),
                Some(Arc::clone(&token)),
            ))));
        }

        Right(HostDispatcher::get_active_adapter(hd.clone()).and_then(
            move |adapter| match adapter {
                Some(adapter) => {
                    let weak_adapter = Arc::downgrade(&adapter);
                    Right(adapter.write().start_discovery().and_then(
                        move |resp| match resp.error {
                            Some(_) => future::ready(Ok((resp, None))),
                            None => {
                                let token = Arc::new(DiscoveryRequestToken { adap: weak_adapter });
                                hd.write().discovery = Some(Arc::downgrade(&token));
                                future::ready(Ok((resp, Some(token))))
                            }
                        },
                    ))
                }
                None => Left(future::ready(Ok((
                    bt_fidl_status!(BluetoothNotAvailable, "No Adapter found"),
                    None,
                )))),
            },
        ))
    }

    pub fn set_discoverable(
        hd: Arc<RwLock<HostDispatcher>>,
    ) -> impl Future<
        Output = fidl::Result<(
            fidl_fuchsia_bluetooth::Status,
            Option<Arc<DiscoverableRequestToken>>,
        )>,
    > {
        let strong_current_token = match hd.read().discoverable {
            Some(ref token) => token.upgrade(),
            None => None,
        };
        if let Some(token) = strong_current_token {
            return Left(future::ready(Ok((
                bt_fidl_status!(),
                Some(Arc::clone(&token)),
            ))));
        }

        Right(HostDispatcher::get_active_adapter(hd.clone()).and_then(
            move |adapter| match adapter {
                Some(adapter) => {
                    let weak_adapter = Arc::downgrade(&adapter);
                    let res =
                        adapter
                            .write()
                            .set_discoverable(true)
                            .and_then(move |resp| match resp.error {
                                Some(_) => future::ready(Ok((resp, None))),
                                None => {
                                    let token =
                                        Arc::new(DiscoverableRequestToken { adap: weak_adapter });
                                    hd.write().discoverable = Some(Arc::downgrade(&token));
                                    future::ready(Ok((resp, Some(token))))
                                }
                            });
                    Right(res)
                }
                None => Left(future::ready(Ok((
                    bt_fidl_status!(BluetoothNotAvailable, "No Adapter found"),
                    None,
                )))),
            },
        ))
    }

    pub fn set_active_adapter(&mut self, adapter_id: String) -> fidl_fuchsia_bluetooth::Status {
        if let Some(ref id) = self.active_id {
            if *id == adapter_id {
                return bt_fidl_status!(Already, "Adapter already active");
            }

            // Shut down the previously active host.
            let _ = self.host_devices[id].write().close();
        }

        if self.host_devices.contains_key(&adapter_id) {
            self.set_active_id(Some(adapter_id));
            bt_fidl_status!()
        } else {
            bt_fidl_status!(NotFound, "Attempting to activate an unknown adapter")
        }
    }

    pub fn get_active_adapter_info(&mut self) -> Option<AdapterInfo> {
        match self.get_active_id() {
            Some(ref id) => {
                // Id must always be valid
                let host = self.host_devices.get(id).unwrap().read();
                Some(util::clone_host_info(host.get_info()))
            }
            None => None,
        }
    }

    pub fn connect_le_central(
        hd: Arc<RwLock<HostDispatcher>>,
    ) -> impl Future<Output = fidl::Result<Option<CentralProxy>>> {
        OnAdaptersFound::new(hd.clone()).and_then(|hd| {
            let mut hd = hd.write();
            future::ready(Ok(match hd.get_active_id() {
                Some(ref id) => {
                    let host = hd.host_devices.get(id).unwrap();
                    Some(host.write().connect_le_central().unwrap())
                }
                None => None,
            }))
        })
    }

    pub fn connect(
        hd: Arc<RwLock<HostDispatcher>>,
        device_id: String,
    ) -> impl Future<Output = fidl::Result<fidl_fuchsia_bluetooth::Status>> {
        HostDispatcher::connect_le_central(hd.clone()).and_then(move |central| {
            let (service_local, service_remote) = fidl::endpoints::create_proxy().unwrap();

            let central = central.unwrap();
            let connected = central.connect_peripheral(device_id.as_str(), service_remote);
            connected.and_then(move |status| {
                let host = hd.clone();
                // TODO(NET-1092): We want this as a host.fidl API
                HostDispatcher::get_active_adapter(host).and_then(move |adapter| match adapter {
                    Some(adapter) => {
                        adapter
                            .write()
                            .store_gatt(device_id, central, service_local);
                        future::ready(Ok(status))
                    }
                    None => future::ready(Ok(bt_fidl_status!(
                        BluetoothNotAvailable,
                        "Adapter went away"
                    ))),
                })
            })
        })
    }

    pub fn forget(
        _hd: Arc<RwLock<HostDispatcher>>,
        _device_id: String,
    ) -> impl Future<Output = fidl::Result<fidl_fuchsia_bluetooth::Status>> {
        // TODO(NET-1148): This function should perform the following:
        // 1. Remove the device from bt-gap's in-memory list of devices, once it exists.
        // 2. Remove bonding data from store::Stash.
        // 3. Call Host.Forget(), once it exists.
        future::ready(Ok(bt_fidl_status!(NotSupported, "Operation not supported")))
    }

    pub fn disconnect(
        hd: Arc<RwLock<HostDispatcher>>,
        device_id: String,
    ) -> impl Future<Output = fidl::Result<fidl_fuchsia_bluetooth::Status>> {
        HostDispatcher::get_active_adapter(hd).and_then(move |adapter| match adapter {
            Some(adapter) => Right(adapter.write().rm_gatt(device_id)),
            None => Left(future::ready(Ok(bt_fidl_status!(
                BluetoothNotAvailable,
                "Adapter went away"
            )))),
        })
    }

    pub fn get_active_adapter(
        hd: Arc<RwLock<HostDispatcher>>,
    ) -> impl Future<Output = fidl::Result<Option<Arc<RwLock<HostDevice>>>>> {
        OnAdaptersFound::new(hd.clone()).and_then(|hd| {
            let mut hd = hd.write();
            future::ready(Ok(match hd.get_active_id() {
                Some(ref id) => Some(hd.host_devices.get(id).unwrap().clone()),
                None => None,
            }))
        })
    }

    pub fn get_adapters(
        hd: &mut Arc<RwLock<HostDispatcher>>,
    ) -> impl Future<Output = fidl::Result<Vec<AdapterInfo>>> {
        OnAdaptersFound::new(hd.clone()).and_then(|hd| {
            let mut result = vec![];
            for host in hd.read().host_devices.values() {
                let host = host.read();
                result.push(util::clone_host_info(host.get_info()));
            }
            future::ready(Ok(result))
        })
    }

    // Resolves all pending OnAdapterFuture's. Called when we leave the init period (by seeing the
    // first host device or when the init timer expires).
    fn resolve_host_requests(&mut self) {
        for waker in &self.host_requests {
            waker.1.wake();
        }
    }

    fn add_host(&mut self, id: String, host: Arc<RwLock<HostDevice>>) {
        self.host_devices.insert(id, host);
    }

    // Updates the active adapter and sends a FIDL event.
    fn set_active_id(&mut self, id: Option<String>) {
        fx_log_info!("New active adapter: {:?}", id);
        self.active_id = id;
        if let Some(ref mut adapter_info) = self.get_active_adapter_info() {
            for events in self.event_listeners.iter() {
                let _res = events.send_on_active_adapter_changed(Some(OutOfLine(adapter_info)));
            }
        }
    }
}

/// A future that completes when at least one adapter is available.
#[must_use = "futures do nothing unless polled"]
struct OnAdaptersFound {
    hd: Arc<RwLock<HostDispatcher>>,
    waker_key: Option<usize>,
}

impl OnAdaptersFound {
    // Constructs an OnAdaptersFound that completes at the latest after HOST_INIT_TIMEOUT seconds.
    fn new(
        hd: Arc<RwLock<HostDispatcher>>,
    ) -> impl Future<Output = fidl::Result<Arc<RwLock<HostDispatcher>>>> {
        OnAdaptersFound {
            hd: hd.clone(),
            waker_key: None,
        }.on_timeout(
            Duration::from_seconds(HOST_INIT_TIMEOUT).after_now(),
            move || {
                {
                    let mut hd = hd.write();
                    if hd.host_devices.len() == 0 {
                        fx_log_info!("No bt-host devices found");
                        hd.resolve_host_requests();
                    }
                }
                Ok(hd)
            },
        )
    }

    fn remove_waker(&mut self) {
        if let Some(key) = self.waker_key {
            self.hd.write().host_requests.remove(key);
        }
        self.waker_key = None;
    }
}

impl Drop for OnAdaptersFound {
    fn drop(&mut self) {
        self.remove_waker()
    }
}

impl Unpin for OnAdaptersFound {}

impl Future for OnAdaptersFound {
    type Output = fidl::Result<Arc<RwLock<HostDispatcher>>>;

    fn poll(mut self: ::std::pin::Pin<&mut Self>, lw: &LocalWaker) -> Poll<Self::Output> {
        if self.hd.read().host_devices.len() == 0 {
            let hd = self.hd.clone();
            if self.waker_key.is_none() {
                self.waker_key = Some(hd.write().host_requests.insert(lw.clone().into_waker()));
            }
            Poll::Pending
        } else {
            self.remove_waker();
            Poll::Ready(Ok(self.hd.clone()))
        }
    }
}

/// Adds an adapter to the host dispatcher. Called by the watch_hosts device
/// watcher
async fn add_adapter(hd: Arc<RwLock<HostDispatcher>>, host_path: PathBuf) -> Result<(), Error> {
    fx_log_info!("Adding Adapter: {:?}", host_path);

    // Connect to the host device.
    let host =
        File::open(host_path.clone()).map_err(|_| BTError::new("failed to open bt-host device"))?;
    let handle = bt::host::open_host_channel(&host)?;
    let handle = fasync::Channel::from_channel(handle.into())?;
    let host = HostProxy::new(handle);

    // Obtain basic information and create and entry in the disptacher's map.
    let adapter_info = await!(host.get_info())
        .map_err(|_| BTError::new("failed to obtain bt-host information"))?;
    let id = adapter_info.identifier.clone();
    let address = adapter_info.address.clone();
    let host_device = Arc::new(RwLock::new(HostDevice::new(host_path, host, adapter_info)));

    // Load bonding data that use this host's `address` as their "local identity address".
    if let Some(iter) = hd.read().stash.list_bonds(&address) {
        if let Err(e) = await!(
            host_device
                .read()
                .restore_bonds(iter.map(|bd| util::clone_bonding_data(&bd)).collect())
        ) {
            fx_log_err!("failed to restore bonding data for host: {}", e);
            return Err(e.into());
        }
    }

    // TODO(NET-1445): Only the active host should be made connectable and scanning in the
    // background.
    await!(host_device.read().set_connectable(true))
        .map_err(|_| BTError::new("failed to set connectable"))?;
    host_device
        .read()
        .enable_background_scan(true)
        .map_err(|_| BTError::new("failed to enable background scan"))?;

    // Initialize bt-gap as this host's pairing delegate.
    // TODO(NET-1445): Do this only for the active host. This will make sure that non-active hosts
    // always reject pairing.
    let (delegate_local, delegate_remote) = zx::Channel::create()?;
    let delegate_local = fasync::Channel::from_channel(delegate_local)?;
    let delegate_ptr = fidl::endpoints::ClientEnd::<PairingDelegateMarker>::new(delegate_remote);
    host_device
        .read()
        .set_host_pairing_delegate(hd.read().input, hd.read().output, delegate_ptr);
    fasync::spawn(
        services::start_pairing_delegate(hd.clone(), delegate_local)
            .unwrap_or_else(|e| eprintln!("Failed to spawn {:?}", e)),
    );

    fx_log_info!("Host added: {:?}", host_device.read().get_info().identifier);
    hd.write().add_host(id, host_device.clone());

    // Notify Control interface clients about the new device.
    // TODO(armansito): This layering isn't quite right. It's better to do this in
    // HostDispatcher::add_host instead.
    for listener in hd.read().event_listeners.iter() {
        let _res =
            listener.send_on_adapter_updated(&mut clone_host_info(host_device.read().get_info()));
    }

    // Resolve pending adapter futures.
    hd.write().resolve_host_requests();

    // Start listening to Host interface events.
    await!(host_device::run(hd.clone(), host_device.clone()))
        .map_err(|_| BTError::new("Host interface event stream error").into())
}

pub fn rm_adapter(
    hd: Arc<RwLock<HostDispatcher>>,
    host_path: PathBuf,
) -> impl Future<Output = Result<(), Error>> {
    fx_log_info!("Host removed: {:?}", host_path);

    let mut hd = hd.write();
    let active_id = hd.active_id.clone();

    // Get the host IDs that match `host_path`.
    let ids: Vec<String> = hd
        .host_devices
        .iter()
        .filter(|(_, ref host)| host.read().path == host_path)
        .map(|(k, _)| k.clone())
        .collect();
    for id in &ids {
        hd.host_devices.remove(id);
    }

    // Reset the active ID if it got removed.
    if let Some(active_id) = active_id {
        if ids.contains(&active_id) {
            hd.active_id = None;
        }
    }

    // Try to assign a new active adapter. This may send an "OnActiveAdapterChanged" event.
    if hd.active_id.is_none() {
        let _ = hd.get_active_id();
    }

    future::ready(Ok(()))
}

pub fn watch_hosts(hd: Arc<RwLock<HostDispatcher>>) -> impl Future<Output = Result<(), Error>> {
    let dev = File::open(&BT_HOST_DIR);
    let watcher = vfs_watcher::Watcher::new(&dev.unwrap()).unwrap();
    watcher
        .try_for_each(move |msg| {
            let path = PathBuf::from(format!(
                "{}/{}",
                BT_HOST_DIR,
                msg.filename.to_string_lossy()
            ));
            match msg.event {
                vfs_watcher::WatchEvent::EXISTING | vfs_watcher::WatchEvent::ADD_FILE => {
                    fx_log_info!("Adding device from {:?}", path);
                    Left(Left(add_adapter(hd.clone(), path).map_err(|e| {
                        io::Error::new(io::ErrorKind::Other, e.to_string())
                    })))
                }
                vfs_watcher::WatchEvent::REMOVE_FILE => {
                    fx_log_info!("Removing device from {:?}", path);
                    Left(Right(rm_adapter(hd.clone(), path).map_err(|e| {
                        io::Error::new(io::ErrorKind::Other, e.to_string())
                    })))
                }
                vfs_watcher::WatchEvent::IDLE => {
                    fx_log_info!("HostDispatcher is IDLE");
                    Right(future::ready(Ok(())))
                }
                e => {
                    fx_log_warn!("Unrecognized host watch event: {:?}", e);
                    Right(future::ready(Ok(())))
                }
            }
        }).map_err(|e| e.into())
}
