/* Host stand-in for the RZI error numbers used by the decoder. */
#ifndef RZI_ERR_H
#define RZI_ERR_H

enum {
	RZI_ERR_INVALID = 1000,
	RZI_ERR_ISR = 1001,
	RZI_ERR_NOT_SUPPORTED = 1005,
	RZI_ERR_TOO_LARGE = 1009,
	RZI_ERR_BAD_MESSAGE = 1013,
};

#endif
