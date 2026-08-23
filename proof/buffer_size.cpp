// Does the subscriber buffer depth actually change anything?
//
// `iox2_port_factory_subscriber_builder_set_buffer_size` is new to `iceoryx2.sigs`. Adding a
// name to that list makes it callable; it does not make it effective, and a setter that is
// silently ignored looks exactly like one that works until the day the difference matters.
//
// So this measures the property the setting is for: a publisher that sends more than the
// subscriber can hold overwrites the oldest samples, and a subscriber that never reads until
// the end should therefore recover AT MOST its buffer depth. Two depths are run and compared
// against each other and against the number sent.
//
// THIS IS A RING, NOT A QUEUE, AND THAT IS THE POINT. Loss here is the design: a display
// redrawing at tens of hertz behind a producer running at thousands must drop stale samples
// rather than block the producer. The check is that the DEPTH is respected, not that nothing
// is lost.
//
// SPDX-License-Identifier: Apache-2.0
#include "iox2_api.h"
#include "weft/bus.hpp"
#include "weft/snapshot.hpp"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int failures = 0;
int checks = 0;

void check(bool ok, const std::string &what) {
	checks++;
	std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
	if (!ok) {
		failures++;
	}
}

// Sends `send_count` samples with nobody reading, then drains. Returns how many came back,
// or -1 if the bus refused something along the way.
int run_depth(uint64_t depth, int send_count) {
	iox2_node_h node = nullptr;
	if (iox2_node_builder_create(iox2_node_builder_new(nullptr), nullptr,
				iox2_service_type_e_IPC, &node) != IOX2_OK) {
		return -1;
	}

	// A distinct service per depth. Reusing one name across both runs would open the second
	// against the first's already-created service, and iceoryx2 keeps the settings of
	// whichever port created it -- so the second measurement would silently be the first's.
	const std::string name = "weft/harness/buffer_size/" + std::to_string(depth);
	iox2_service_name_h svc = nullptr;
	if (iox2_service_name_new(nullptr, name.c_str(), name.size(), &svc) != IOX2_OK) {
		iox2_node_drop(node);
		return -1;
	}
	auto builder = iox2_service_builder_pub_sub(
			iox2_node_service_builder(&node, nullptr, iox2_cast_service_name_ptr(svc)));
	// THE SERVICE CAPS WHAT A SUBSCRIBER MAY ASK FOR, and its default is 2. Setting only the
	// subscriber's depth is not enough: a subscriber requesting more than the service allows
	// fails to be created at all, which surfaces as a null handle rather than as a clamped
	// buffer. Measured -- a depth of 16 against an unconfigured service returned no
	// subscriber, while a depth of 1 worked, so the first version of this proof read as "the
	// setter does nothing" when the setter was fine and the service was the limit.
	iox2_service_builder_pub_sub_set_subscriber_max_buffer_size(&builder, depth);
	if (iox2_service_builder_pub_sub_set_payload_type_details(&builder,
				iox2_type_variant_e_FIXED_SIZE, weft::PAYLOAD_TYPE, std::strlen(weft::PAYLOAD_TYPE),
				sizeof(weft::Snapshot), alignof(weft::Snapshot)) != IOX2_OK) {
		iox2_service_name_drop(svc);
		iox2_node_drop(node);
		return -1;
	}
	iox2_port_factory_pub_sub_h service = nullptr;
	if (iox2_service_builder_pub_sub_open_or_create(builder, nullptr, &service) != IOX2_OK) {
		iox2_service_name_drop(svc);
		iox2_node_drop(node);
		return -1;
	}

	// The subscriber is created FIRST. A publisher with no subscriber attached has nowhere
	// to deliver to, and every sample would be dropped for a reason that has nothing to do
	// with the buffer depth under test.
	auto sub_builder = iox2_port_factory_pub_sub_subscriber_builder(&service, nullptr);
	iox2_port_factory_subscriber_builder_set_buffer_size(&sub_builder, depth);
	iox2_subscriber_h sub = nullptr;
	if (iox2_port_factory_subscriber_builder_create(sub_builder, nullptr, &sub) != IOX2_OK) {
		return -1;
	}

	iox2_publisher_h pub = nullptr;
	if (iox2_port_factory_publisher_builder_create(
				iox2_port_factory_pub_sub_publisher_builder(&service, nullptr), nullptr, &pub) !=
			IOX2_OK) {
		return -1;
	}

	for (int i = 0; i < send_count; i++) {
		iox2_sample_mut_h sample = nullptr;
		if (iox2_publisher_loan_slice_uninit(&pub, nullptr, &sample, 1) != IOX2_OK) {
			// No loan means the publisher's own pool is exhausted, which is a different
			// limit from the subscriber's buffer and would make the count meaningless.
			break;
		}
		void *payload = nullptr;
		size_t elements = 0;
		iox2_sample_mut_payload_mut(&sample, &payload, &elements);
		weft::Snapshot s{};
		s.tick = uint64_t(i);
		std::memcpy(payload, &s, sizeof(s));
		(void)iox2_sample_mut_send(sample, nullptr);
	}

	int received = 0;
	for (;;) {
		iox2_sample_h sample = nullptr;
		if (iox2_subscriber_receive(&sub, nullptr, &sample) != IOX2_OK || !sample) {
			break;
		}
		iox2_sample_drop(sample);
		received++;
	}

	iox2_publisher_drop(pub);
	iox2_subscriber_drop(sub);
	iox2_port_factory_pub_sub_drop(service);
	iox2_service_name_drop(svc);
	iox2_node_drop(node);
	return received;
}

} // namespace

int main() {
	if (!weft::load_bus()) {
		std::fprintf(stderr, "buffer_size: no bus\n");
		return 1;
	}
	iox2_set_log_level_from_env_or(iox2_log_level_e_ERROR);

	// Send far more than either depth, so both are saturated and the recovered count is the
	// depth rather than the send count.
	constexpr int SENT = 64;
	const int shallow = run_depth(1, SENT);
	const int deep = run_depth(16, SENT);

	std::printf("sent %d with nobody reading\n", SENT);
	std::printf("  buffer size  1  ->  %d recovered\n", shallow);
	std::printf("  buffer size 16  ->  %d recovered\n", deep);
	std::printf("\n");

	check(shallow >= 0 && deep >= 0, "both runs completed");
	// The bound is what the setting promises. Exceeding it means the depth was ignored.
	check(shallow <= 1, "a depth of 1 holds at most 1");
	check(deep <= 16, "a depth of 16 holds at most 16");

	// THE NEGATIVE CONTROL FOR THE SETTER ITSELF. If the call were ignored, both runs would
	// hold the same default and this would fail -- which is exactly what the check is for.
	// Without it, two runs that both returned the library default would read as a pass.
	check(deep > shallow, "a deeper buffer holds strictly more, so the setter is not ignored");

	// And the sent count is not the answer either, or the buffer is not bounding anything.
	check(deep < SENT, "the deeper buffer still drops, so it is a ring rather than a queue");

	std::printf("\n%d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
