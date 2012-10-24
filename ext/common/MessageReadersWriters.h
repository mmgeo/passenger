/*
 *  Phusion Passenger - http://www.modrails.com/
 *  Copyright (c) 2010 Phusion
 *
 *  "Phusion Passenger" is a trademark of Hongli Lai & Ninh Bui.
 *
 *  See LICENSE file for license information.
 */
#ifndef _PASSENGER_MESSAGE_READERS_WRITERS_H_
#define _PASSENGER_MESSAGE_READERS_WRITERS_H_

#include <boost/cstdint.hpp>
#include <oxt/macros.hpp>
#include <algorithm>
#include <vector>
#include <string>
#include <sys/types.h>
#include <cstring>
#include <arpa/inet.h>
#include <StaticString.h>
#include <Exceptions.h>
#include <Utils/MemZeroGuard.h>

/**
 * This file provides a bunch of classes for reading and writing messages in the
 * MessageIO format. Unlike MessageIO functions, whose operations take control over
 * the I/O handle and may block, these classes act like parsers and data generators.
 * To read messages one must feed data to them. To write messages one must instruct
 * the classes to generate a bunch of data. These classes will never block, making
 * them ideal for use in evented servers.
 *
 * <h2>Reading messages</h2>
 * To read a single message, one must instantiate a message object and feed network
 * data to it with the feed() method. This method returns the number of bytes
 * actually processed by the message object (i.e. the number of bytes that it has
 * recognized as part of the message).
 *
 * When the message object has either successfully parsed the data or encountered
 * an error, it will indicate so via the done() method. With hasError() one can
 * check whether an error was encountered or whether the reader succeeded, and
 * with errorCode() one can obtain the exact error reason. Not all message objects
 * support hasError() and errorCode() because some of them can never encounter
 * errors and some of them can only fail for a single reason.
 *
 * When successful, the parsed message can be obtained with value(). This method
 * may only be called when done() is true and there is no error, otherwise the
 * return value is undefined.
 *
 * At this point, the message object cannot process any more data and feed() will
 * always return 0. To reuse the object for processing another message, one must
 * reset its state by calling reset().
 *
 * The following example demonstrates how to read a continuous stream of 32-bit
 * integers:
 * @code
   Uint32Message intMessage;
   while (true) {
       // Read a bunch of network data...
       char buf[1024];
       ssize_t size = recv(fd, buf, sizeof(buf));
       size_t consumed = 0;
       
       // ...and process it all. We only feed data to the message object
       // that hasn't already been fed.
       while (consumed < size) {
           consumed += intReader.feed(buf + consumed, size - consumed);
           if (intMessage.done()) {
               printf("Integer: %d\n", (int) intMessage.value());
               // The state must be reset before the reader can be reused.
               intMessage.reset();
           }
       }
   }
 * @endcode
 *
 * Some message objects return non-primitive values in their value() methods, such as
 * ArrayMessage and ScalarMessage which return <tt>const vector<StaticString> &</tt>
 * and <tt>StaticString</tt>, respectively. These values are only valid until either
 * of the following things occur:
 *
 * - The buffer containing last the fed data has been destroyed or modified.
 * - The message object itself has been destroyed.
 *
 * This is because the message objects try to apply copy-zero optimizations whenever
 * possible. For example, in case of ScalarMessage, it'll check whether the data
 * that has been fed in the first feed() call already contains a full scalar message.
 * If so then it'll just return a StaticString that points to the scalar message
 * in the fed data; it will not copy the fed data. In this case it is important
 * that the buffer containing the fed data is not modified or destroyed while the
 * StaticString is in use.
 * If the first feed() call does not supply a full scalar message then it will
 * buffer all fed data until the buffer contains a full scalar message, and the
 * result will point to this buffer. Because the buffer is owned by the message
 * object, the result will be invalidated as soon as the message object is destroyed.
 */

namespace Passenger {

using namespace std;

/**
 * For 16-bit big-endian integers.
 */
class Uint16Message {
private:
	uint16_t val;
	uint8_t  consumed;
	
public:
	Uint16Message() {
		consumed = 0;
	}
	
	void reset() {
		consumed = 0;
	}
	
	size_t feed(const char *data, size_t size) {
		size_t locallyConsumed;
		
		locallyConsumed = std::min(size, sizeof(uint16_t) - consumed);
		memcpy((char *) &val + consumed, data, locallyConsumed);
		consumed += locallyConsumed;
		if (locallyConsumed > 0 && done()) {
			val = ntohs(val);
		}
		return locallyConsumed;
	}
	
	bool done() const {
		return consumed == sizeof(uint16_t);
	}
	
	uint16_t value() const {
		return val;
	}
	
	static void generate(void *buf, uint16_t val) {
		val = htons(val);
		memcpy(buf, &val, sizeof(val));
	}
};

/**
 * For 32-bit big-endian integers.
 */
class Uint32Message {
private:
	uint32_t val;
	uint8_t  consumed;
	
public:
	Uint32Message() {
		consumed = 0;
	}
	
	void reset() {
		consumed = 0;
	}
	
	size_t feed(const char *data, size_t size) {
		size_t locallyConsumed;
		
		locallyConsumed = std::min(size, sizeof(uint32_t) - consumed);
		memcpy((char *) &val + consumed, data, locallyConsumed);
		consumed += locallyConsumed;
		if (locallyConsumed > 0 && done()) {
			val = ntohl(val);
		}
		return locallyConsumed;
	}
	
	bool done() const {
		return consumed == sizeof(uint32_t);
	}
	
	uint32_t value() const {
		return val;
	}
	
	static void generate(void *buf, uint32_t val) {
		val = htonl(val);
		memcpy(buf, &val, sizeof(val));
	}
};

/**
 * For array messages.
 */
class ArrayMessage {
public:
	enum Error {
		TOO_LARGE
	};
	
private:
	enum State {
		READING_HEADER,
		READING_BODY,
		DONE,
		ERROR
	};
	
	uint16_t toReserve;
	uint16_t maxSize;
	Uint16Message headerReader;
	uint8_t state;
	uint8_t error;
	string buffer;
	vector<StaticString> result;
	
	void parseBody(const char *data, size_t size) {
		const char *start = data;
		const char *terminator;
		size_t rest = size;
		
		while ((terminator = (const char *) memchr(start, '\0', rest)) != NULL) {
			size_t len = terminator - start;
			result.push_back(StaticString(start, len));
			start = terminator + 1;
			rest = size - (start - data);
		}
	}
	
public:
	ArrayMessage() {
		state = READING_HEADER;
		toReserve = 0;
		maxSize = 0;
	}
	
	void reserve(uint16_t size) {
		toReserve = size;
		result.reserve(size);
	}
	
	void setMaxSize(uint16_t size) {
		maxSize = size;
	}
	
	void reset() {
		state = READING_HEADER;
		headerReader.reset();
		buffer.clear();
		result.clear();
		if (toReserve > 0) {
			result.reserve(toReserve);
		}
	}
	
	size_t feed(const char *data, size_t size) {
		size_t consumed = 0;
		
		while (consumed < size && !done()) {
			const char *current = data + consumed;
			size_t rest = size - consumed;
			
			switch (state) {
			case READING_HEADER:
				consumed += headerReader.feed(current, rest);
				if (headerReader.done()) {
					if (maxSize > 0 && headerReader.value() > maxSize) {
						state = ERROR;
						error = TOO_LARGE;
					} else if (headerReader.value() == 0) {
						state = DONE;
					} else {
						state = READING_BODY;
					}
				}
				break;
			case READING_BODY:
				if (buffer.empty() && rest >= headerReader.value()) {
					parseBody(current, headerReader.value());
					state = DONE;
					consumed += headerReader.value();
				} else {
					size_t toConsume = std::min(rest,
						headerReader.value() - buffer.size());
					if (buffer.capacity() < headerReader.value()) {
						buffer.reserve(headerReader.value());
					}
					buffer.append(current, toConsume);
					consumed += toConsume;
					if (buffer.size() == headerReader.value()) {
						parseBody(buffer.data(), buffer.size());
						state = DONE;
					}
				}
				break;
			default:
				// Never reached.
				abort();
			}
		}
		return consumed;
	}
	
	bool done() const {
		return state == DONE || state == ERROR;
	}
	
	bool hasError() const {
		return state == ERROR;
	}
	
	Error errorCode() const {
		return (Error) error;
	}
	
	const vector<StaticString> &value() const {
		return result;
	}
	
	/**
	 * Given a bunch of array items, generates an array message. The message is
	 * generated in the form of an array of StaticStrings which must all be written
	 * out (e.g. with writev()) in the given order. These StaticStrings point
	 * to the buffers pointed to by <em>args</em> as well as <em>headerBuf</em>,
	 * so <em>args</em> and <em>headerBuf</em> must stay valid until the message
	 * has been written out or copied.
	 *
	 * @param args An array of array items to be included in the array message.
	 * @param argsCount The number of items in <em>args</em>.
	 * @param headerBuf A pointer to a buffer in which the array message header
	 *                  is to be stored.
	 * @param out A pointer to a StaticString array in which the generated array
	 *            message data will be stored. Exactly <tt>outputSize(argsCount)</tt>
	 *            items will be stored in this array.
	 * @param outCount The number of items in <em>out</em>.
	 */
	static void generate(StaticString args[], unsigned int argsCount,
		char headerBuf[sizeof(uint16_t)], StaticString *out, unsigned int outCount)
	{
		if (OXT_UNLIKELY(outCount < outputSize(argsCount))) {
			throw ArgumentException("outCount too small.");
		}
		
		unsigned int size = 0;
		unsigned int i;
		
		for (i = 0; i < argsCount; i++) {
			size += args[i].size() + 1;
		}
		if (OXT_UNLIKELY(size > 0xFFFF)) {
			throw ArgumentException("Data size exceeds maximum size for array messages.");
		}
		
		Uint16Message::generate(headerBuf, size);
		out[0] = StaticString(headerBuf, sizeof(uint16_t));
		for (i = 0; i < argsCount; i++) {
			out[1 + 2 * i] = args[i];
			out[1 + 2 * i + 1] = StaticString("\0", 1);
		}
	}
	
	static unsigned int outputSize(unsigned int argsCount) {
		return argsCount * 2 + 1;
	}
};

/**
 * Class for reading a scalar message.
 */
class ScalarMessage {
public:
	enum Error {
		TOO_LARGE
	};
	
private:
	enum State {
		READING_HEADER,
		READING_BODY,
		DONE,
		ERROR
	};
	
	uint8_t state;
	uint8_t error;
	uint32_t maxSize;
	Uint32Message headerReader;
	string buffer;
	StaticString result;
	
public:
	ScalarMessage(uint32_t maxSize = 0) {
		state = READING_HEADER;
		this->maxSize = maxSize;
	}
	
	void setMaxSize(uint32_t maxSize) {
		this->maxSize = maxSize;
	}
	
	/**
	 * Resets the internal state so that this object can be used for processing
	 * another scalar message.
	 *
	 * If <em>zeroBuffer</em> is true, then the contents of the internal buffer
	 * will be securely filled with zeroes. This is useful if e.g. the buffer
	 * might contain sensitive password data.
	 */
	void reset(bool zeroBuffer = false) {
		state = READING_HEADER;
		if (zeroBuffer) {
			MemZeroGuard guard(buffer);
		}
		headerReader.reset();
		buffer.clear();
	}
	
	size_t feed(const char *data, size_t size) {
		size_t consumed = 0;
		
		while (consumed < size && !done()) {
			const char *current = data + consumed;
			size_t rest = size - consumed;
			
			switch (state) {
			case READING_HEADER:
				consumed += headerReader.feed(current, rest);
				if (headerReader.done()) {
					if (maxSize > 0 && headerReader.value() > maxSize) {
						state = ERROR;
						error = TOO_LARGE;
					} else if (headerReader.value() == 0) {
						state = DONE;
					} else {
						state = READING_BODY;
					}
				}
				break;
			case READING_BODY:
				if (buffer.empty() && rest >= headerReader.value()) {
					result = StaticString(current, headerReader.value());
					state = DONE;
					consumed += headerReader.value();
				} else {
					size_t toConsume = std::min(rest,
						headerReader.value() - buffer.size());
					if (buffer.capacity() < headerReader.value()) {
						buffer.reserve(headerReader.value());
					}
					buffer.append(current, toConsume);
					consumed += toConsume;
					if (buffer.size() == headerReader.value()) {
						result = StaticString(buffer);
						state = DONE;
					}
				}
				break;
			default:
				// Never reached.
				abort();
			};
		}
		return consumed;
	}
	
	bool done() const {
		return state == DONE || state == ERROR;
	}
	
	bool hasError() const {
		return state == ERROR;
	}
	
	Error errorCode() const {
		return (Error) error;
	}
	
	const StaticString &value() const {
		return result;
	}
	
	static void generate(const StaticString &data, char headerBuf[sizeof(uint32_t)],
		StaticString output[2])
	{
		if (OXT_UNLIKELY(data.size() > 0xFFFFFFFF)) {
			throw ArgumentException("Data size exceeds maximum size for scalar messages.");
		}
		
		Uint32Message::generate(headerBuf, data.size());
		output[0] = StaticString(headerBuf, sizeof(uint32_t));
		output[1] = data;
	}

	// output must be at least count + 1 in length
	static void generate(const StaticString data[], unsigned int count,
		char headerBuf[sizeof(uint32_t)], StaticString *output)
	{
		unsigned int i;
		uint32_t totalSize = 0;

		for (i = 0; i < count; i++) {
			if (OXT_UNLIKELY(data[i].size() > 0xFFFFFFFF)) {
				throw ArgumentException("Data size exceeds maximum size for scalar messages.");
			}
			totalSize += data[i].size();
			output[i + 1] = data[i];
		}

		Uint32Message::generate(headerBuf, totalSize);
		output[0] = StaticString(headerBuf, sizeof(uint32_t));
	}
};

} // namespace Passenger

#endif /* _PASSENGER_MESSAGE_READERS_WRITERS_H_ */
