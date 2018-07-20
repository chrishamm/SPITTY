/*
 * LinuxMessageFormats.h
 *
 *  Created on: 16 Jul 2018
 *      Author: Christian
 */

#ifndef SRC_LINUXMESSAGEFORMATS_H_
#define SRC_LINUXMESSAGEFORMATS_H_

#include <stddef.h>
#include <stdint.h>

// *** NOTE: THIS IS PRELIMINARY AND NOT INTENDED FOR LONG-TIME USAGE! DEV-ONLY ***

// Message formats exchanges over the SPI link between the SAME70 processor and an arbitrary Linux board
// The Linux board is the SPI master because it takes control of the attached board.
// At this time the Linux board keeps polling for status updates / G-code replies in regular intervals
// but this *may* be changed again in the future

// First the message header formats
const size_t MaxDataLength = 2048;				// maximum length of the data part of an SPI exchange

#ifdef __cplusplus
static_assert(MaxDataLength % sizeof(uint32_t) == 0, "MaxDataLength must be a whole number of dwords");
#endif

const uint8_t LinuxFormatVersion = 0x8F;
const uint8_t InvalidFormatVersion = 0xC9;		// must be different from any format version we have ever used

// Commands from the Linux board to the SAM
#ifdef __cplusplus
enum class LinuxRequest : uint8_t
#else
typedef uint8_t LinuxRequest;

enum
#endif
{
	nullCommand = 0,				// no command being sent

	doGCode,						// perform a new G-code
	getGCodeReply,					// retrieve the last G-code

	emergencyStop					// perform an emergency stop
};

// Message header sent from the Linux board to the SAM
struct MessageHeaderLinuxToSam
{
	uint8_t formatVersion;
	LinuxRequest request;			// see above
	uint8_t dummy[2];
	uint16_t dataLength;			// how long the data part of the request is
	uint16_t dummy2;
};

// Message header sent from the Linux board to the SAM
// Note that the last word is sent concurrently with the response from the Linux board, so it doesn't get seen by the Linux board before it decides what response to send
struct MessageHeaderSamToLinux
{
	uint8_t formatVersion;
	uint8_t dummy[3];
	int32_t response;				// response length if positive, or error code if negative
};

#ifdef __cplusplus
static_assert(sizeof(MessageHeaderSamToLinux) == sizeof(MessageHeaderLinuxToSam), "Message header sizes don't match");
#endif

// Response error codes. A non-negative code is the number of bytes of returned data.
const int32_t ResponseEmpty = 0;				// used when there is no error and no data to return
const int32_t ResponseUnknownCommand = -1;
const int32_t ResponseBadHeaderVersion = -2;
const int32_t ResponseBadDataLength = -3;
const int32_t ResponseBusy = -4;
const int32_t ResponseBadReplyFormatVersion = -5;
const int32_t ResponseUnknownError = -6;

#endif /* SRC_LINUXMESSAGEFORMATS_H_ */
