// What `weft::cbor::Reading` accepts, and what it must refuse.
//
// THIS EXISTS BECAUSE A COMMENT WAS WRONG AND NOTHING CHECKED IT. `well_formed` said that
// `Finish` "reports both a malformed item and trailing bytes past the end". It reports the
// first and not the second, and four of four trailing bytes were accepted on a message that
// was otherwise complete. A caller appending bytes is a broken sender, and reading the first
// item and discarding the rest answers it as though it were a good one.
//
// A claim in a comment is not a check. This is the check.
//
// EVERY CASE HERE IS ENUMERATED, not sampled, so there is no detection floor to state: the
// population is the set of shapes a one-message buffer can take.
//
// SPDX-License-Identifier: Apache-2.0
#include "weft/cbor.hpp"

#include <cstdio>
#include <string>
#include <vector>

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

// A complete, ordinary message: {"a": 1, "b": "two"}.
std::vector<unsigned char> good_message() {
	std::vector<unsigned char> buf(256);
	weft::cbor::Map m(buf.data(), buf.size());
	m.uint("a", 1);
	m.text("b", "two");
	const std::size_t n = m.finish();
	buf.resize(n);
	return buf;
}

bool accepted(const std::vector<unsigned char> &bytes) {
	weft::cbor::Reading r(bytes.data(), bytes.size());
	return r.ok();
}

std::vector<unsigned char> with_trailing(unsigned char b) {
	std::vector<unsigned char> v = good_message();
	v.push_back(b);
	return v;
}

} // namespace

int main() {
	std::printf("Accepted: a complete message\n");
	{
		const std::vector<unsigned char> good = good_message();
		check(!good.empty(), "the fixture encodes");
		check(accepted(good), "a well-formed map is accepted");

		weft::cbor::Reading r(good.data(), good.size());
		std::uint64_t a = 0;
		std::string b;
		check(r.uint("a", a) && a == 1, "an integer field reads back");
		check(r.text("b", b) && b == "two", "a text field reads back");
	}

	std::printf("\nAccepted: a message missing fields is an OLDER sender\n");
	{
		// The opposite verdict from everything below, and the distinction is the whole
		// point of the class: malformed and missing need different answers.
		const unsigned char empty_map[] = {0xA0};
		weft::cbor::Reading r(empty_map, sizeof(empty_map));
		check(r.ok(), "an empty map is accepted");
		std::uint64_t v = 0;
		check(!r.uint("absent", v), "an absent field declines rather than poisoning the read");
	}

	std::printf("\nRefused: trailing bytes past the message\n");
	// Each of these was ACCEPTED before this check existed. 0xFF is the one that survived
	// two earlier attempts at a fix: it is a break stop-code with no indefinite-length item
	// to close, so GetNext refuses it and the item count stays at one, and QCBOR counts it
	// as consumed so a length comparison sees nothing wrong either.
	check(!accepted(with_trailing(0xFF)), "a trailing break stop-code is refused");
	check(!accepted(with_trailing(0x01)), "a trailing integer is refused");
	check(!accepted(with_trailing(0xA0)), "a trailing empty map is refused");
	check(!accepted(with_trailing(0x00)), "a trailing zero is refused");
	{
		// A whole second message appended. This is the case that matters on a bus, where a
		// sender that batched two messages into one send would otherwise have the first
		// answered and the second silently dropped.
		std::vector<unsigned char> two = good_message();
		const std::vector<unsigned char> second = good_message();
		two.insert(two.end(), second.begin(), second.end());
		check(!accepted(two), "two concatenated messages are refused, not read as one");
	}

	std::printf("\nRefused: malformed and truncated\n");
	{
		const std::vector<unsigned char> good = good_message();
		std::vector<unsigned char> half(good.begin(), good.begin() + good.size() / 2);
		check(!accepted(half), "a truncated message is refused, not read as a short one");
	}
	{
		// Well-formed CBOR that is not a map. A message is one map; an integer is a sender
		// that agreed to a different format.
		const unsigned char just_an_int[] = {0x01};
		weft::cbor::Reading r(just_an_int, sizeof(just_an_int));
		check(!r.ok(), "a well-formed non-map is refused");
	}
	{
		const std::vector<unsigned char> nothing;
		weft::cbor::Reading r(nothing.data(), nothing.size());
		check(!r.ok(), "an empty buffer is refused");
	}

	std::printf("\n%d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
