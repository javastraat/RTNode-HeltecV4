#include "Interface.h"

#include "Identity.h"
#include "Transport.h"
#include "Reticulum.h"
#include "Cryptography/Hashes.h"
#include "Cryptography/HKDF.h"

using namespace RNS;
using namespace RNS::Type::Interface;

/*static*/ uint8_t Interface::DISCOVER_PATHS_FOR = MODE_FULL | MODE_ACCESS_POINT | MODE_GATEWAY | MODE_BOUNDARY | MODE_ROAMING;

void Interface::setup_ifac(const char* ifac_netname, const char* ifac_netkey) {
	assert(_impl);
	if (ifac_netname == nullptr && ifac_netkey == nullptr) {
		return;
	}
	// If both are empty strings, treat as no IFAC
	bool has_netname = (ifac_netname != nullptr && ifac_netname[0] != '\0');
	bool has_netkey = (ifac_netkey != nullptr && ifac_netkey[0] != '\0');
	if (!has_netname && !has_netkey) {
		return;
	}

	TRACE("Interface::setup_ifac: setting up IFAC for " + _impl->_name);

	// Build ifac_origin = SHA256(netname) || SHA256(netkey)
	Bytes ifac_origin;
	if (has_netname) {
		Bytes netname_bytes((const uint8_t*)ifac_netname, strlen(ifac_netname));
		Bytes hash = Identity::full_hash(netname_bytes);
		ifac_origin = ifac_origin + hash;
	}
	if (has_netkey) {
		Bytes netkey_bytes((const uint8_t*)ifac_netkey, strlen(ifac_netkey));
		Bytes hash = Identity::full_hash(netkey_bytes);
		ifac_origin = ifac_origin + hash;
	}

	// Hash the combined origin
	Bytes ifac_origin_hash = Identity::full_hash(ifac_origin);

	// Derive ifac_key via HKDF(salt=IFAC_SALT, ikm=ifac_origin_hash, length=64)
	Bytes salt(IFAC_SALT, IFAC_SALT_SIZE);
	_impl->_ifac_key = Cryptography::hkdf(64, ifac_origin_hash, salt);

	// Create an identity from the derived key (64 bytes = 32 X25519 + 32 Ed25519)
	Identity ifac_id(false);  // don't auto-generate keys
	ifac_id.load_private_key(_impl->_ifac_key);
	_impl->_ifac_id = ifac_id;

	// Set _ifac_identity to non-empty to flag IFAC as enabled
	// (Transport checks this with operator bool)
	_impl->_ifac_identity = ifac_id.get_public_key();

	TRACE("Interface::setup_ifac: IFAC configured, ifac_size=" + std::to_string(_impl->_ifac_size));
}

void InterfaceImpl::handle_outgoing(const Bytes& data) {
	//TRACE("InterfaceImpl.handle_outgoing: data: " + data.toHex());
	TRACE("InterfaceImpl.handle_outgoing");
	_txb += data.size();
}

void InterfaceImpl::handle_incoming(const Bytes& data) {
	//TRACE("InterfaceImpl.handle_incoming: data: " + data.toHex());
	TRACE("InterfaceImpl.handle_incoming");
	_rxb += data.size();
	// Create temporary Interface encapsulating our own shared impl
	std::shared_ptr<InterfaceImpl> self = shared_from_this();
	Interface interface(self);
	// Pass data on to transport for handling
	Transport::inbound(data, interface);
}

void Interface::handle_incoming(const Bytes& data) {
	//TRACE("Interface.handle_incoming: data: " + data.toHex());
	TRACE("Interface.handle_incoming");
	assert(_impl);
/*
	_impl->_rxb += data.size();
	// Pass data on to transport for handling
	Transport::inbound(data, *this);
*/
	_impl->handle_incoming(data);
}

void Interface::process_announce_queue() {
/*
	if not hasattr(self, "announce_cap"):
		self.announce_cap = RNS.Reticulum.ANNOUNCE_CAP

	if hasattr(self, "announce_queue"):
		try:
			now = time.time()
			stale = []
			for a in self.announce_queue:
				if now > a["time"]+RNS.Reticulum.QUEUED_ANNOUNCE_LIFE:
					stale.append(a)

			for s in stale:
				if s in self.announce_queue:
					self.announce_queue.remove(s)

			if len(self.announce_queue) > 0:
				min_hops = min(entry["hops"] for entry in self.announce_queue)
				entries = list(filter(lambda e: e["hops"] == min_hops, self.announce_queue))
				entries.sort(key=lambda e: e["time"])
				selected = entries[0]

				double now = OS::time();
				uint32_t wait_time = 0;
				if (_impl->_bitrate > 0 && _impl->_announce_cap > 0) {
					uint32_t tx_time = (len(selected["raw"])*8) / _impl->_bitrate;
					wait_time = (tx_time / _impl->_announce_cap);
				}
				_impl->_announce_allowed_at = now + wait_time;

				self.on_outgoing(selected["raw"])

				if selected in self.announce_queue:
					self.announce_queue.remove(selected)

				if len(self.announce_queue) > 0:
					timer = threading.Timer(wait_time, self.process_announce_queue)
					timer.start()

		except Exception as e:
			self.announce_queue = []
			RNS.log("Error while processing announce queue on "+str(self)+". The contained exception was: "+str(e), RNS.LOG_ERROR)
			RNS.log("The announce queue for this interface has been cleared.", RNS.LOG_ERROR)
*/
}

/*
void ArduinoJson::convertFromJson(JsonVariantConst src, RNS::Interface& dst) {
	TRACE(">>> Deserializing Interface");
TRACE(">>> Interface pre: " + dst.debugString());
	if (!src.isNull()) {
		RNS::Bytes hash;
		hash.assignHex(src.as<const char*>());
		TRACE(">>> Querying Transport for Interface hash " + hash.toHex());
		// Query transport for matching interface
		dst = Transport::find_interface_from_hash(hash);
TRACE(">>> Interface post: " + dst.debugString());
	}
	else {
		dst = {RNS::Type::NONE};
TRACE(">>> Interface post: " + dst.debugString());
	}
}
*/

// ── Ingress control, as Python RNS Interface ─────────────────────────────────
// Announces for destinations not yet in the path table are held while their
// source is bursting (Transport::inbound decides), then released one at a time
// once it calms down (Transport::loop releases). Constants: Type.h.

static std::string ingress_label(const Interface& interface, const IngressState& state) {
	if (state.client < 0) { return interface.toString(); }
	return interface.toString() + " client " + std::to_string(state.client);
}

// Announces per second over the last IA_FREQ_SAMPLES arrivals, counting the
// time since the last one — Python's incoming_announce_frequency().
static double incoming_announce_frequency(const IngressState& state, double now) {
	uint8_t count = state.ia_count;
	if (count < 2) { return 0; }
	// Oldest sample first: the ring's next write slot once it is full.
	uint8_t oldest = (count < IA_FREQ_SAMPLES) ? 0 : state.ia_next;
	double delta_sum = 0;
	for (uint8_t i = 1; i < count; i++) {
		delta_sum += state.ia_times[(oldest + i) % IA_FREQ_SAMPLES] - state.ia_times[(oldest + i - 1) % IA_FREQ_SAMPLES];
	}
	delta_sum += now - state.ia_times[(oldest + count - 1) % IA_FREQ_SAMPLES];
	if (delta_sum == 0) { return 0; }
	return 1.0 / (delta_sum / count);
}

static double burst_threshold(const IngressState& state, double now) {
	return (now - state.created < IC_NEW_TIME) ? IC_BURST_FREQ_NEW : IC_BURST_FREQ;
}

// Python's should_ingress_limit().
static bool should_limit(const Interface& interface, IngressState& state, double now) {
	double threshold = burst_threshold(state, now);
	double ia_freq = incoming_announce_frequency(state, now);
	if (state.burst_active) {
		if (ia_freq < threshold && now > state.burst_activated + IC_BURST_HOLD) {
			state.burst_active = false;
			state.held_release = now + IC_BURST_PENALTY;
			NOTICEF("[INGRESS] %s burst over, %u announces held, release in %us",
			        ingress_label(interface, state).c_str(), (unsigned)state.held.size(), (unsigned)IC_BURST_PENALTY);
		}
		return true;
	}
	if (ia_freq > threshold) {
		state.burst_active = true;
		state.burst_activated = now;
		NOTICEF("[INGRESS] %s burst: %.1f announces/s > %.1f, holding new destinations",
		        ingress_label(interface, state).c_str(), ia_freq, threshold);
		return true;
	}
	return false;
}

void Interface::received_announce() {
	assert(_impl);
	IngressState& state = _impl->ingress_source();
	double now = Utilities::OS::time();
	// Age for the new-interface threshold counts from the first announce:
	// OS::time() jumps when Reticulum restores its saved time offset, after
	// interfaces are registered, so a registration timestamp would make every
	// interface look hours old.
	if (state.created == 0) { state.created = now; }
	state.ia_times[state.ia_next] = now;
	state.ia_next = (state.ia_next + 1) % IA_FREQ_SAMPLES;
	if (state.ia_count < IA_FREQ_SAMPLES) { state.ia_count++; }
}

bool Interface::should_ingress_limit() {
	assert(_impl);
	return should_limit(*this, _impl->ingress_source(), Utilities::OS::time());
}

void Interface::hold_announce(const Bytes& destination_hash, const Bytes& raw, uint8_t hops) {
	assert(_impl);
	IngressState& state = _impl->ingress_source();
	for (auto& held : state.held) {
		if (held.destination_hash == destination_hash) {
			held.raw = raw;
			held.hops = hops;
			return;
		}
	}
	// Python holds up to 256; that fits in PSRAM, not in a board's internal RAM.
	size_t limit = Utilities::OS::heap_in_psram() ? MAX_HELD_ANNOUNCES : MAX_HELD_ANNOUNCES_SMALL;
	if (state.held.size() >= limit) { return; }
	HeldAnnounce held;
	held.destination_hash = destination_hash;
	held.raw = raw;
	held.hops = hops;
	state.held.push_back(held);
}

// Python's process_held_announces(), minus the inbound() call: one held
// announce (fewest hops first) from each source that has calmed down.
void Interface::collect_released_announces(std::vector<Bytes>& out) {
	assert(_impl);
	double now = Utilities::OS::time();
	_impl->for_each_ingress([&](IngressState& state) {
		if (state.held.empty() || should_limit(*this, state, now)) { return; }
		if (now <= state.held_release) { return; }
		if (incoming_announce_frequency(state, now) >= burst_threshold(state, now)) { return; }
		size_t selected = state.held.size();
		uint8_t min_hops = Type::Transport::PATHFINDER_M;
		for (size_t i = 0; i < state.held.size(); i++) {
			if (state.held[i].hops < min_hops) {
				min_hops = state.held[i].hops;
				selected = i;
			}
		}
		if (selected == state.held.size()) { return; }
		state.held_release = now + IC_HELD_RELEASE_INTERVAL;
		out.push_back(state.held[selected].raw);
		state.held.erase(state.held.begin() + selected);
	});
}

size_t Interface::held_announce_count() const {
	assert(_impl);
	size_t held = 0;
	_impl->for_each_ingress([&](IngressState& state) { held += state.held.size(); });
	return held;
}
