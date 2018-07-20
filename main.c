/* SPITTY v1.0
 *
 * written by Christian Hammacher, (c) 2018
 * licensed under the terms of the GPLv2
 *
 * This program is intended as proxy service between a Raspberry PI
 * and a Duet 3D printer controller running RepRapFirmware over SPI.
 *
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>

#include "LinuxMessageFormats.h"


// SPI settings
const char *DefaultSPIPath = "/dev/spidev0.0";
uint8_t spiMode = 0;
uint8_t spiBits = 8;
uint32_t spiSpeed = 500000;

int spiExchangeHeaders(int fd, struct MessageHeaderLinuxToSam *tx, struct MessageHeaderSamToLinux *rx);
int spiWriteRead(int fd, char * const tx, char * const rx, size_t len);

// SPI communication
const unsigned int DefaultSpiTransactionDelay = 20;		// ms
int spiTransactionDelay;
struct MessageHeaderLinuxToSam hdrToSend;
struct MessageHeaderSamToLinux hdrToRecv;

// Socket settings
const char *DefaultFifoPath = "/dev/duet0";
const unsigned int DefaultUpdateInterval = 200;			// ms

// UNIX signal handlig
bool exitRequested = false;
void termHandler(int signum)
{
	exitRequested = true;
}

int main(int argc, char *argv[])
{
	printf("SPITTY v1.0\n");

	// Process parameters
	const char *spiPath = DefaultSPIPath;
	const char *fifoPath = DefaultFifoPath;
	int updateInterval = DefaultUpdateInterval;
	spiTransactionDelay = DefaultSpiTransactionDelay;

	for (size_t i = 1; i < argc; i++)
	{
		// TODO
		if (!strcmp(argv[i], "--help"))
		{
			printf("--help\t\t\tShow this list\n");
		}
	}

	// Open and set up SPI device
	int spi = open(spiPath, O_RDWR);
	if (spi < 0)
	{
		fprintf(stderr, "Failed to open SPI device\n");
		return 1;
	}
	if (ioctl(spi, SPI_IOC_WR_MODE, &spiMode) < 0 && ioctl(spi, SPI_IOC_RD_MODE, &spiMode) < 0)
	{
		fprintf(stderr, "Failed to set SPI mode\n");
		return 1;
	}
	if (ioctl(spi, SPI_IOC_WR_BITS_PER_WORD, &spiBits) < 0 || ioctl(spi, SPI_IOC_RD_BITS_PER_WORD, &spiBits) < 0)
	{
		fprintf(stderr, "Failed to set SPI word length\n");
		return 1;
	}
	if (ioctl(spi, SPI_IOC_WR_MAX_SPEED_HZ, &spiSpeed) < 0 || ioctl(spi, SPI_IOC_RD_MAX_SPEED_HZ, &spiSpeed) < 0)
	{
		fprintf(stderr, "Failed to set SPI speed\n");
		return 1;
	}
	printf("Using SPI device %s (mode %d, word length %d bits, speed %d kHz)\n", spiPath, spiMode, spiBits, spiSpeed / 1000);

	// Check if a Duet is actually connected to this SPI port
	printf("Performing handshake with RepRapFirmware... ");
	hdrToSend.formatVersion = LinuxFormatVersion;
	hdrToSend.request = nullCommand;
	hdrToSend.dataLength = 0;
	hdrToRecv.formatVersion = InvalidFormatVersion;
	int result = spiExchangeHeaders(spi, &hdrToSend, &hdrToRecv);
	if (result < 0)
	{
		printf("Error\n");
		fprintf(stderr, "Failed to transmit data (err %d)\n", errno);
		return 2;
	}
	if (hdrToRecv.formatVersion != LinuxFormatVersion)
	{
		printf("Error\n");
		fprintf(stderr, "Invalid format version\n");
		return 3;
	}
	if (hdrToRecv.response != 0)
	{
		printf("Error\n");
		fprintf(stderr, "Invalid response\n");
		return 4;
	}
	printf("Success\n");

	// Create the named pipes
	unlink(fifoPath);
	if (mkfifo(fifoPath, 0666) < 0)
	{
		fprintf(stderr, "Failed to create named pipe\n");
		return 5;
	}

	const int fifo = open(fifoPath, O_RDWR | O_NONBLOCK);
	if (fifo < 0)
	{
		fprintf(stderr, "Failed to open named pipe\n");
		unlink(fifoPath);
		return 6;
	}
	printf("Using FIFO %s\n", fifoPath);

	// Set up signal handlers
	signal(SIGINT, termHandler);
	signal(SIGHUP, termHandler);
	signal(SIGTERM, termHandler);

	// Keep reading from the FIFO and ask RRF for status updates in regular intervals
	char buffer[MaxDataLength];
	if (buffer == NULL)
	{
		fprintf(stderr, "Failed to allocate buffer\n");
		close(spi);
		close(fifo);
		unlink(fifoPath);
		return 7;
	}

	do
	{
		// Is there any new data available on the FIFO?
		ssize_t bytesRead = read(fifo, buffer, MaxDataLength);
		if (bytesRead <= 0)
		{
			// No
			if (errno == EAGAIN)
			{
				// There is no data available, wait a moment
				usleep(updateInterval * 1000);
			}
			else
			{
				fprintf(stderr, "Failed to read from FIFO (err %d)", errno);
				break;
			}
		}
		else
		{
			// Yes - send it to the Duet
			bool busy = false, hadError = false;
			do
			{
				hdrToSend.request = doGCode;
				hdrToSend.dataLength = bytesRead;
				hdrToRecv.formatVersion = InvalidFormatVersion;
				result = spiExchangeHeaders(spi, &hdrToSend, &hdrToRecv);
				if (result < 0)
				{
					hadError = true;
					fprintf(stderr, "Failed to read from SPI (err %d)\n", errno);
				}
				else if (hdrToRecv.response == ResponseBusy)
				{
					busy = true;
					usleep(updateInterval * 1000);
				}
				else if (hdrToRecv.response != ResponseEmpty)
				{
					hadError = true;
					fprintf(stderr, "Received invalid response (%d)\n", hdrToRecv.response);
				}
				else
				{
					busy = false;
					printf("TX: %s", buffer);
					spiWriteRead(spi, buffer, buffer, bytesRead);
				}
			} while (busy && !hadError);

			if (hadError)
			{
				break;
			}
		}

		// Is there any new G-code response? Send a new request
		hdrToSend.request = getGCodeReply;
		hdrToSend.dataLength = 0;
		hdrToRecv.formatVersion = InvalidFormatVersion;
		result = spiExchangeHeaders(spi, &hdrToSend, &hdrToRecv);
		if (result < 0)
		{
			fprintf(stderr, "Failed to read from SPI (err %d)\n", errno);
			break;
		}

		// Get the response
		result = spiExchangeHeaders(spi, &hdrToSend, &hdrToRecv);
		if (result < 0)
		{
			fprintf(stderr, "Failed to read from SPI (err %d)\n", errno);
			break;
		}
		else if (hdrToRecv.response != ResponseBusy)
		{
			if (hdrToRecv.response < 0)
			{
				fprintf(stderr, "Received invalid response (%d)\n", hdrToRecv.response);
				break;
			}
			else if (hdrToRecv.response != 0)
			{
				char response[hdrToRecv.response + 1];
				result = spiWriteRead(spi, response, response, hdrToRecv.response);
				if (result < 0)
				{
					fprintf(stderr, "Failed to read from SPI (err %d)\n", errno);
				}
				else if (hdrToRecv.response < 0)
				{
					fprintf(stderr, "Received invalid response (%d)\n", hdrToRecv.response);
					break;
				}
				else
				{
					response[hdrToRecv.response] = 0;
					printf("RX: %s", response);
					write(fifo, response, hdrToRecv.response);
				}
			}
		}
	}
	while (!exitRequested);

	// Clean up again
	close(spi);
	close(fifo);
	unlink(fifoPath);
	return 0;
}

int spiExchangeHeaders(int fd, struct MessageHeaderLinuxToSam * const tx, struct MessageHeaderSamToLinux * const rx)
{
	return spiWriteRead(fd, (char * const)tx, (char * const)rx, sizeof(struct MessageHeaderLinuxToSam));
}

int spiWriteRead(int fd, char * const tx, char * const rx, size_t len)
{
	usleep(spiTransactionDelay * 1000);
	struct spi_ioc_transfer tr = {
		.tx_buf = (unsigned long)tx,
		.rx_buf = (unsigned long)rx,
		.len = len,
		.speed_hz = spiSpeed,
		.delay_usecs = 0,
		.bits_per_word = spiBits
	};
	return ioctl(fd, SPI_IOC_MESSAGE(1), &tr);
}

