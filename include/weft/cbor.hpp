// The reply format, and the library that reads it.
//
// `transport-bus-cli`'s decoder opens by saying the interactors write CBOR, and handles four
// kinds -- maps, text, byte strings and integers -- on the argument that a decoder handling
// everything RFC 8949 allows would be a library, and a reply needing one is a reply whose
// shape nobody agreed on. The agreement is worth keeping. The parser is not worth writing.
//
// QCBOR does the parsing. It allocates nothing: encoding writes into a caller's buffer and
// decoding reads in place, so leaks, double frees and reference-count errors are absent rather
// than avoided. That is the property this file is about, and it was chosen against a live
// example -- a first wrapper written over a reference-counted CBOR library dropped a map entry
// silently, because the add call reported failure through a return value it was easy to ignore.
// Here the equivalent failure is recorded in the encode context and reported once, at the end,
// by a function whose result is the encoded buffer.
//
// What stays here is the agreement, not an implementation of it: the four kinds, plus doubles
// and booleans, and one place that decides what a malformed message does.
//
// SPDX-License-Identifier: Apache-2.0
#ifndef WEFT_CBOR_HPP
#define WEFT_CBOR_HPP

#include <qcbor/qcbor_decode.h>
#include <qcbor/qcbor_encode.h>
#include <qcbor/qcbor_spiffy_decode.h>

#include <cstdint>
#include <cstring>
#include <string>

namespace weft::cbor {

// A map written straight into the caller's buffer. No allocation, and no intermediate tree:
// every Add appends bytes, and an overflow is remembered rather than thrown.
class Map {
public:
	Map(unsigned char *out, std::size_t cap) {
		QCBOREncode_Init(&ctx_, UsefulBuf{out, cap});
		QCBOREncode_OpenMap(&ctx_);
	}

	void uint(const char *key, std::uint64_t v) { QCBOREncode_AddUInt64ToMap(&ctx_, key, v); }
	void boolean(const char *key, bool v) { QCBOREncode_AddBoolToMap(&ctx_, key, v); }
	void text(const char *key, const char *v) { QCBOREncode_AddSZStringToMap(&ctx_, key, v); }

	// Doubles as doubles. Coordinates are doubles through the whole mesher, and rounding them
	// to cross the bus would put a precision decision in the transport. QCBOREncode_AddDouble
	// does not shorten to a half or single float, so what is written is what was held.
	void real(const char *key, double v) { QCBOREncode_AddDoubleToMap(&ctx_, key, v); }

	void bytes(const char *key, const void *p, std::size_t n) {
		QCBOREncode_AddBytesToMap(&ctx_, key, UsefulBufC{p, n});
	}

	// The encoded length, or 0 if anything went wrong -- an overflowed buffer, an unclosed map,
	// a key too long. One check at the end rather than one per field, because QCBOR carries the
	// first error forward and refuses to report success over it. A short write can therefore
	// never be mistaken for a short message.
	std::size_t finish() {
		QCBOREncode_CloseMap(&ctx_);
		UsefulBufC done{nullptr, 0};
		return QCBOREncode_Finish(&ctx_, &done) == QCBOR_SUCCESS ? done.len : 0;
	}

private:
	QCBOREncodeContext ctx_{};
};

// A map read in place. Nothing is copied and nothing is owned, so the bytes must outlive it.
//
// Every getter is looked up by label rather than by position, which is what makes the format
// an agreement instead of an ordering: a reply that gains a field does not move the others.
class Reading {
public:
	Reading(const void *data, std::size_t n) {
		ok_ = well_formed(data, n);
		if (!ok_) return;
		QCBORDecode_Init(&ctx_, UsefulBufC{data, n}, QCBOR_DECODE_MODE_NORMAL);
		QCBORDecode_EnterMap(&ctx_, nullptr);
		ok_ = QCBORDecode_GetError(&ctx_) == QCBOR_SUCCESS;
	}

	// True only if the message was a well-formed map. A truncated or malformed message is
	// refused here rather than surfacing as a missing field later, because the two need
	// different answers: one is a broken sender, the other is an older one.
	bool ok() const { return ok_; }

	bool text(const char *key, std::string &out) {
		UsefulBufC s{nullptr, 0};
		QCBORDecode_GetTextStringInMapSZ(&ctx_, key, &s);
		if (!clear()) return false;
		out.assign(static_cast<const char *>(s.ptr), s.len);
		return true;
	}

	bool uint(const char *key, std::uint64_t &out) {
		QCBORDecode_GetUInt64InMapSZ(&ctx_, key, &out);
		return clear();
	}

	bool boolean(const char *key, bool &out) {
		QCBORDecode_GetBoolInMapSZ(&ctx_, key, &out);
		return clear();
	}

	bool real(const char *key, double &out) {
		QCBORDecode_GetDoubleInMapSZ(&ctx_, key, &out);
		return clear();
	}

	bool bytes(const char *key, const unsigned char *&at, std::size_t &n) {
		UsefulBufC b{nullptr, 0};
		QCBORDecode_GetByteStringInMapSZ(&ctx_, key, &b);
		if (!clear()) return false;
		at = static_cast<const unsigned char *>(b.ptr);
		n = b.len;
		return true;
	}

private:
	// QCBOR decodes lazily: entering a map succeeds on a truncated buffer, and the shortfall
	// surfaces only when a read runs off the end. That is right for a decoder walking a file
	// and wrong for one reading a message, where truncated and complete need different
	// answers -- a short message is a broken sender, a missing field is an older one.
	//
	// So the whole message is walked once before anything is read from it, and the walk has
	// to answer THREE questions rather than one.
	//
	// CORRECTED: THIS USED TO CLAIM `Finish` REPORTED TRAILING BYTES. It does not, and the
	// comment saying so shipped. `QCBORDecode_GetNext` is called until it stops succeeding,
	// which CONSUMES a trailing item rather than tripping over it, so `Finish` then sees a
	// cleanly exhausted buffer and reports success. Measured on a 99-byte encoded map with
	// one byte appended:
	//
	//     trailing 0xFF break stop-code   accepted
	//     trailing 0x01 integer 1         accepted
	//     trailing 0xA0 empty map         accepted
	//     trailing 0x00 integer 0         accepted
	//
	// Four of four, where the comment promised a refusal. A caller appending bytes to a
	// message is a broken sender, and reading the first item and discarding the rest answers
	// it as though it were a good one.
	//
	// The fix took three attempts and the two that failed are recorded here, because each
	// looked complete:
	//
	//     input          top-level items   consumed   Finish   stopped because
	//     {"a":1}                      1        4/4       ok    NO_MORE_ITEMS
	//     {"a":1} + 01                 2        5/5       ok    NO_MORE_ITEMS
	//     {"a":1} + FF                 1        5/5       ok    malformed
	//
	// Counting top-level items misses the last row: a stray break is refused by GetNext, so
	// the walk stops at one item with the byte still there. Comparing the consumed length
	// misses it too, because QCBOR counts the break as consumed. Only the REASON the walk
	// stopped separates that row from the first, and only the COUNT separates the second.
	// Both are needed, which is why neither alone was enough.
	static bool well_formed(const void *data, std::size_t n) {
		QCBORDecodeContext c{};
		QCBORDecode_Init(&c, UsefulBufC{data, n}, QCBOR_DECODE_MODE_NORMAL);

		QCBORItem item;
		int top_level = 0;
		QCBORError stopped_because = QCBOR_SUCCESS;
		for (;;) {
			stopped_because = QCBORDecode_GetNext(&c, &item);
			if (stopped_because != QCBOR_SUCCESS) {
				break;
			}
			// A map's own item is at nesting level 0 and its members are at level 1, so one
			// top-level item is exactly one message however deeply it nests.
			if (item.uNestingLevel == 0) {
				top_level++;
			}
		}

		// The walk must have run out of items, rather than stopped on one it could not read.
		if (stopped_because != QCBOR_ERR_NO_MORE_ITEMS) {
			return false;
		}
		if (QCBORDecode_Finish(&c) != QCBOR_SUCCESS) {
			return false;
		}
		return top_level == 1;
	}

	// A failed lookup leaves the error set, and QCBOR then refuses every later call so one
	// missing field would silently fail the rest. Reading a field is asking a question, and a
	// no is an answer, so the error is taken and cleared here.
	bool clear() {
		const QCBORError e = QCBORDecode_GetAndResetError(&ctx_);
		return e == QCBOR_SUCCESS;
	}

	QCBORDecodeContext ctx_{};
	bool ok_ = false;
};

}  // namespace weft::cbor

#endif
