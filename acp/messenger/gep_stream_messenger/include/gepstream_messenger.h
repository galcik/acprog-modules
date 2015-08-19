#ifndef MODULES_ACP_MESSENGER_GEP_STREAM_MESSENGER_INCLUDE_GEPSTREAM_MESSENGER_H_
#define MODULES_ACP_MESSENGER_GEP_STREAM_MESSENGER_INCLUDE_GEPSTREAM_MESSENGER_H_

#include <acp/core.h>

namespace acp_messenger_gep_stream {

	template <int MAX_MESSAGE_SIZE> class TGEPStreamMessenger;

	// Byte indicating start of a new message
	const uint8_t MESSAGE_START_BYTE = 0x0C;

	// Byte indicating end of the message without tag
	const uint8_t MESSAGE_END_BYTE = 0x03;

	// Byte indicating end of the message with tag
	const uint8_t MESSAGE_END_WITH_TAG_BYTE = 0x06;

	/********************************************************************************
	 * Controller for a stream messenger using a GEP protocol: error checking protocol
	 * based on http://www.gammon.com.au/forum/?id=11428
	 ********************************************************************************/
	template<int MAX_MESSAGE_SIZE> class GEPStreamController {
		friend class TGEPStreamMessenger<MAX_MESSAGE_SIZE>;
	private:
		// Communication stream for sending and receiving messages.
		Stream* stream;

		// Buffer for receiving messages
		uint8_t message[MAX_MESSAGE_SIZE+2];

		// Number of received message bytes
		int messageLength;

		// State of the receive process
		enum {WAIT_START, WAIT_MESSAGE_BYTE_HIGH, WAIT_MESSAGE_BYTE_LOW, WAIT_CRC, WAIT_CRC_WITH_TAG, MESSAGE_RECEIVED, MESSAGE_RECEIVED_WITH_TAG}state;

		//--------------------------------------------------------------------------------
		// Computes CRC checksum of given data
		uint8_t computeCRC8(uint8_t crc, const uint8_t *data, int dataLength) {
			while (dataLength > 0) {
				uint8_t inByte = *data;
				for (uint8_t i = 8; i>0; i--) {
					uint8_t mix = (crc ^ inByte) & 0x01;
					crc >>= 1;
					if (mix) {
						crc ^= 0x8C;
					}
					inByte >>= 1;
				}

				dataLength--;
				data++;
			}

			return crc;
		}

		//--------------------------------------------------------------------------------
		// Sends a byte encoded as two nibbles
		inline void sendByte(uint8_t dataByte) {
			uint8_t encodedByte[2];
			uint8_t nibble;

			// Encode high nibble
			nibble = dataByte / 16;
			encodedByte[0] = (nibble << 4) | (nibble ^ 0x0F);

			// Encode low nibble
			nibble = dataByte % 16;
			encodedByte[1] = (nibble << 4) | (nibble ^ 0x0F);

			stream->write(encodedByte, 2);
		}

		//--------------------------------------------------------------------------------
		// Sends a message (if tag is negative, no tag is attached to the message)
		void sendMessage(const char* message, int messageLength, long tag) {
			if ((stream == NULL) || (messageLength < 0) || ((messageLength > 0) && (message == NULL))) {
				return;
			}

			// Send encoded message content
			stream->write(MESSAGE_START_BYTE);
			const char* msgPtr = message;
			for (int i=0; i<messageLength; i++) {
				sendByte(*msgPtr);
				msgPtr++;
			}

			// Compute CRC checksum
			uint8_t crcChecksum = computeCRC8(0, (const uint8_t*)message, messageLength);

			// Send tail of message (eventually with encoded tag)
			if (tag < 0) {
				stream->write(MESSAGE_END_BYTE);
			} else {
				uint8_t tagBuffer[2];
				tagBuffer[0] = tag / 256;
				tagBuffer[1] = tag % 256;
				sendByte(tagBuffer[0]);
				sendByte(tagBuffer[1]);
				crcChecksum = computeCRC8(crcChecksum, tagBuffer, 2);
				stream->write(MESSAGE_END_WITH_TAG_BYTE);
			}

			// Send CRC checksum
			stream->write(crcChecksum);
		}

	public:
		// Event handler invoked when a message is received
		void (*messageReceivedEvent)(const char* message, int messageLength, long messageTag);

		//--------------------------------------------------------------------------------
		// Constructs the protocol controller
		inline GEPStreamController() {
			stream = NULL;
			messageReceivedEvent = NULL;
			state = WAIT_START;
		}

		//--------------------------------------------------------------------------------
		// Loop code for reading message data from stream
		void loop() {
			while ((stream != NULL) && (stream->available() > 0)) {
				const int dataByte = stream->read();
				if (dataByte < 0) {
					break;
				}

				// Process waiting - we change state to WAIT_START or break the loop (if a correct message is received)
				if ((state == WAIT_CRC) || (state == WAIT_CRC_WITH_TAG)) {
					// Check CRC of received data
					if (dataByte != computeCRC8(0, (const byte*)message, messageLength)) {
						// Invalid state (reset receive) - invalid checksum
						state = WAIT_START;
					} else {
						if (state == WAIT_CRC_WITH_TAG) {
							messageLength -= 2;
							state = MESSAGE_RECEIVED_WITH_TAG;
						} else {
							// Invalid state (reset receive) - too long message
							if (messageLength > MAX_MESSAGE_SIZE) {
								state = WAIT_START;
							}
							state = MESSAGE_RECEIVED;
						}

						break;
					}
				}

				// After receiving the message start byte, the receive of the message is restarted
				if (dataByte == MESSAGE_START_BYTE) {
					state == WAIT_START;
				}

				if ((state == WAIT_START) && (dataByte == MESSAGE_START_BYTE)) {
					state = WAIT_MESSAGE_BYTE_HIGH;
					messageLength = 0;
					continue;
				}

				if ((state == WAIT_MESSAGE_BYTE_HIGH) && (dataByte == MESSAGE_END_BYTE)) {
					state = WAIT_CRC;
					continue;
				}

				if ((state == WAIT_MESSAGE_BYTE_HIGH) && (dataByte == MESSAGE_END_WITH_TAG_BYTE)) {
					if (messageLength >= 2) {
						state = WAIT_CRC_WITH_TAG;
					} else {
						// Invalid state (reset receive)
						state = WAIT_START;
					}
					continue;
				}

				if ((state == WAIT_MESSAGE_BYTE_HIGH) || (state == WAIT_MESSAGE_BYTE_LOW)) {
					const uint8_t inByte = (uint8_t)dataByte;
					const uint8_t nibble = inByte / 16;

					// Check whether received byte is well formed data byte
					if (nibble != ((inByte ^ 0x0F) & 0x0F)) {
						// Invalid state (reset receive)
						state = WAIT_START;
						continue;
					}

					if (state == WAIT_MESSAGE_BYTE_HIGH) {
						if (messageLength >= MAX_MESSAGE_SIZE+2) {
							// Invalid state (reset receive) - message buffer is full
							state = WAIT_START;
							continue;
						}

						message[messageLength] = nibble * 16;
						messageLength++;
						state = WAIT_MESSAGE_BYTE_LOW;
					} else {
						message[messageLength-1] += nibble;
						state = WAIT_MESSAGE_BYTE_HIGH;
					}

					continue;
				}
			}

			// Notify new message without a tag
			if (state == MESSAGE_RECEIVED) {
				if (messageReceivedEvent != NULL) {
					// Terminate the message with null (in the case when message processor requires it)
					message[messageLength] = 0;
					// Handle message
					messageReceivedEvent((const char*)message, messageLength, -1);
				}
				state = WAIT_START;
			}

			// Notify new message with a tag
			if (state == MESSAGE_RECEIVED_WITH_TAG) {
				if (messageReceivedEvent != NULL) {
					// Compute tag
					const uint8_t* tagStart = message + messageLength;
					const unsigned int tag = tagStart[0] * 256L + tagStart[1];

					// Terminate the message with null (in the case when message processor requires it)
					message[messageLength] = 0;
					// Handle message
					messageReceivedEvent((const char*)message, messageLength, tag);
				}
				state = WAIT_START;
			}
		}
	};

	/********************************************************************************
	 * View for a stream messenger using a GEP protocol
	 ********************************************************************************/
	template<int MAX_MESSAGE_SIZE> class TGEPStreamMessenger {
	private:
		// The controller
		GEPStreamController<MAX_MESSAGE_SIZE>& controller;
	public:
		//--------------------------------------------------------------------------------
		// Constructs view associated with a controller
		inline TGEPStreamMessenger(GEPStreamController<MAX_MESSAGE_SIZE>& controller): controller(controller) {
			// Nothing to do
		}

		//--------------------------------------------------------------------------------
		// Sets stream used for communication
		inline void setStream(Stream& stream) {
			controller.stream = &stream;
		}

		//--------------------------------------------------------------------------------
		// Unsets stream used for communication
		inline void unsetStream() {
			controller.stream = NULL;
		}

		//--------------------------------------------------------------------------------
		// Sends a message without a tag
		inline void sendMessage(const char* message, int messageLength) {
			controller.sendMessage(message, messageLength, -1);
		}

		//--------------------------------------------------------------------------------
		// Sends a message with a tag
		inline void sendMessage(const char* message, int messageLength, unsigned int tag) {
			controller.sendMessage(message, messageLength, tag);
		}
	};

}

#endif /* MODULES_ACP_MESSENGER_GEP_STREAM_MESSENGER_INCLUDE_GEPSTREAM_MESSENGER_H_ */
