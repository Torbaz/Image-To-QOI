/*H**********************************************************************
 * FILENAME :        encodeQOI.c
 *
 * DESCRIPTION :
 *       Image data importing via stb_image (https://github.com/nothings/stb), image encoding and saving
 * 		 according to the specifications listed at https://qoiformat.org/
 *
 * AUTHOR :    Eamon Heffernan        START DATE :    04 May 22
 *
 *H*/

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// Different operating systems have different functions for accessing files.
// Use macro definition to set a function for windows that behaves the same as the POSIX one.
#ifdef _WIN32
inline int access(const char *pathname, int mode)
{
	return _access(pathname, mode);
}
#else
#include <unistd.h>
#endif

// Importing the STB Image library to handle png and jpeg decoding.
// https://github.com/nothings/stb
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

struct Pixel
{
	unsigned char r;
	unsigned char g;
	unsigned char b;
	unsigned char a;
};

struct InputImage
{
	unsigned int width;
	unsigned int height;
	char *fileLocation;
	struct Pixel *pixels;
};

struct OutputImage
{
	unsigned int width;
	unsigned int height;
	char *fileLocation;
	char *data;
	unsigned int dataSize;
};

void waitForInput()
{
	// Flush any previous input from the stream.
	fflush(stdin);

	printf("Press ENTER to continue...");
	// Waits for the enter key.
	getchar();
}

// Compares 2 pixels and determines if all the values are the same.
bool matchingPixels(struct Pixel *p1, struct Pixel *p2)
{
	return p1->r == p2->r && p1->g == p2->g && p1->b == p2->b && p1->a == p2->a;
};

// Hashes a pixel for use as the index in the running array.
// Returns a value between 0 and 63.
int getQOIHash(struct Pixel *p)
{
	// Multiply the r, g, b and a values by the first 4 primes after 2 and
	// get the remainder after division by 64, resulting in a well distributed value
	// between 0 and 63.
	return (p->r * 3 + p->g * 5 + p->b * 7 + p->a * 11) % 64;
};

// Determines if a number is within a certain range of another and returns the difference.
// However it wraps the values at 255.
// 255 + 1 == 0
// 0 - 1 == 255
int withinWrappedRange(int original, int comparison, int minOffset, int maxOffset)
{
	// First check if the numbers are within the range without wrapping.
	if (comparison >= original - minOffset && comparison <= original + maxOffset)
	{
		return comparison - original;
	}

	// Otherwise, check if the original is close enough to 0 for wrapping below 0 to be possible.
	// If it is, then add 256 to the minimum value to wrap the original to above 255.
	if (original - minOffset < 0 && comparison >= 256 + (original - minOffset))
	{
		// Over Wrapped Min
		// Return the difference, accounting for the wrapping.
		return comparison - (original + 256);
	}

	// Otherwise do the same thing for wrapping the original value above 255.
	// If the original is close enough to 255, check that if 256 was subtracted
	// would the comparison value be within the range.
	if (original + maxOffset > 255 && comparison <= original + maxOffset - 256)
	{
		// Under Wrapped Max
		// Return the difference, accounting for the wrapping.
		return comparison - (original - 256);
	}

	// If the value is not in the range even with wrapping, return the min int value which
	// is used as the nil response.
	return INT_MIN;
}

// Writes an int (4 bytes) to the given byte array at the given index.
void writeIntToByteArray(char *bytes, int index, int value)
{
	// Loops through all 4 bytes of the int.
	for (int i = 0; i < 4; i++)
	{
		// For each byte in the int:
		// 1. 	Shift the bits by (3 - i) bytes to the right.
		//		This pushes the byte in question to the very right.
		// 2.	Get the last 8 bits.
		// 3.	Save to the array.
		// Ex.
		//		Value: 11110000111100001111000011110000
		//		Byte: 1
		//		3 - Byte: 2
		//		(3 - Byte) * 8: 16
		//		Move the bits 16 to the right
		//		00000000000000001111000011110000
		//		Get the last 8 bits by finding the bits that are in both the result and OxFF (11111111)
		//		11110000
		//		Save this value to the bytes array
		bytes[index + i] = (value >> ((3 - i) * 8)) & 0xFF;
	}
}

// Saves a run of pixels to the data of the output image.
void saveRun(char *data, unsigned char *run, int *dataIndex)
{
	// A run is used when there are multiple pixels of the same value in a row.
	// The first pixel is saved with a different operation and subsequent pixels are
	// held until the run finishes.
	// All of these pixels may then be saved in one byte with a 2 bit header and six bits
	// to display how many times the pixel is repeated after the first.
	//
	// The run saved is one less than the number of pixels that follow the first as there is no
	// run of 0, saving one value that can be used.
	//
	// The maximum run that is saved is 62. There are 64 values available with 64 bits but using
	// 63 and 64 would interfere with the tags for OP_RGB and OP_RGBA.

	// The value saved to the array is any bits that are in either the run - 1 or the header (0b11000000)
	// As the maximum value that would be sent to this function is 62, this just combines both values into
	// one byte.
	data[*dataIndex] = 0b11000000 | *run - 1;

	*dataIndex = *dataIndex + 1;
	*run = 0;
}

void convertToQOI(struct InputImage *inputImage, struct OutputImage *outputImage)
{
	// There are 14 bytes in the header and 8 in the the footer.
	// 5 bytes is the largest possible size of one pixel.
	// Therefore 5 * the number of pixels + 22 is the maximum size of the array.
	outputImage->data = malloc(5 * inputImage->height * inputImage->width + 22);

	// The running array is used to hold recently used pixel. It behaves as follows:
	// Every pixel can be hashed using the function getQOIHash to get the corresponding index
	// for that pixel value. Each pixel after it is written to the data will be saved at the index.
	// As there are only 64 outputs of the function, collisions are inevitable.
	// However on average, it will take 64 new pixels to be overriden.
	// This array can then be referenced by first checking if the pixel at the index has the same values as
	// the current pixel. If so then a pixel can be saved in 1 byte.
	// This array does not have to be saved with the file as it is reconstructed in the same way when decoding the
	// file.
	// The array is allocated the size of 64 pixels.
	struct Pixel *runningArray = malloc(sizeof(struct Pixel) * 64);

	// Set up the previous pixel with the initial value of (0,0,0,255)
	struct Pixel prevPixel;

	prevPixel.r = 0x00;
	prevPixel.g = 0x00;
	prevPixel.b = 0x00;
	prevPixel.a = 0xFF;

	// 14 Byte QOI File Header
	// QOIF text bytes present on all QOI files.
	outputImage->data[0] = 'q';
	outputImage->data[1] = 'o';
	outputImage->data[2] = 'i';
	outputImage->data[3] = 'f';
	// 4 Bytes that store the image width
	writeIntToByteArray(outputImage->data, 4, inputImage->width);
	// 4 Bytes that store the image height
	writeIntToByteArray(outputImage->data, 8, inputImage->height);
	// The number of channels. (Set at 4 for convenience. Image file size will be the same regardless.)
	outputImage->data[12] = 0x04;
	// The colorspace of the image. (0x00 = SRGB with Linear Alpha, 0x01 All Channels Linear)
	outputImage->data[13] = 0x00;

	// The initial data index is set at 14 as 0-13 are filled by the header.
	// This is used as the pixel index used in the for loop is not bound to the index in the data array.
	int dataIndex = 14;

	unsigned char run = 0;

	// Start pixel at 0, run the loop while it is less than the number of pixels in the file (height*width).
	for (int pixel = 0; pixel < inputImage->height * inputImage->width; pixel++)
	{
		// Shorthand for the pixel at in the input image at the current index.
		struct Pixel currentPixel = inputImage->pixels[pixel];
		// If run > 0 and the current pixel is not the same as the previous pixel, write the run.
		// If All the same then increment run ALSO HANDLE CASE IF RUN > 62
		if (matchingPixels(&currentPixel, &prevPixel))
		{
			if (run == 62)
			{
				// Run is max allowed value.
				// Only 64 values fit in 6 bits (2 taken by tag)
				// Values 63 and 64 would result in a byte that is the same as the tag
				// for OP_RGB and OP_RGBA and therefore cannot be used.

				// Save Run
				saveRun(outputImage->data, &run, &dataIndex);
			}

			// Add to the run.
			// If the run was just saved because run == 62, another run can immediately be started again
			// without needing to save the pixel another way.
			run++;

			// Can continue because changing the prev pixel & array do not need to be changed as this pixel is the same
			// as the last.
			continue;
		}

		if (run > 0)
		{
			// The pixel is not the same as the previous one, however there was an existing run.
			// Save the run before continuing with the current pixel.
			saveRun(outputImage->data, &run, &dataIndex);
		}

		// Get the hash of the current pixel.
		// Used for saving to and reading from the running array.
		unsigned int QOIHash = getQOIHash(&currentPixel);

		// If the pixel value in the array at the index of QOIHash is the same as the current pixel,
		// the index of the pixel can be saved instead of the pixel value. This only uses one byte.
		if (matchingPixels(&currentPixel, &runningArray[QOIHash]))
		{
			// OP_INDEX
			// Save with the tag of 0b00 and the 6 bit QOIHash
			outputImage->data[dataIndex] = 0b00000000 | QOIHash;
			dataIndex++;
		}
		// If the alpha does not match, none of the other operations will work. Skip straight to OP_RGBA
		// which saves the full pixel value including the alpha.
		else if (currentPixel.a == prevPixel.a)
		{
			// Try OP_DIFF
			// To save the pixel value in one byte, the r, g and b values must be at most 2 less or 1 greater
			// that the pixel before (including wrapping).
			// If they are, 2 bits can be dedicated to each part of the pixel (4 values each) and 2 to the tag.
			int dr = withinWrappedRange(prevPixel.r, currentPixel.r, 2, 1);
			int dg = withinWrappedRange(prevPixel.g, currentPixel.g, 2, 1);
			int db = withinWrappedRange(prevPixel.b, currentPixel.b, 2, 1);

			// Check that none of the values returned a invalid response
			if (dr != INT_MIN && dg != INT_MIN && db != INT_MIN)
			{
				// OP_DIFF
				// Save the first 2 bits as the tag, then shift the red to the right as the 3rd and 4th bits,
				// then shift the green to the right as the 5th and 6th bits, with the blue as the last 2 bits.
				// The values are offset by 2 so that -2 becomes 0 and 1 becomes 3, fitting all values within 2 bits.
				outputImage->data[dataIndex] = 0b01000000 | (dr + 2) << 4 | (dg + 2) << 2 | (db + 2);
				dataIndex++;
			}
			else
			{
				// Try OP_LUMA
				// OP_LUMA can save a pixel value in 2 bytes if certain criteria are met.
				// 1.	The difference between the previous pixel's green value and the current pixel
				//		is at most 32 below and 31 above.
				// 2.	The difference between the previous and current red and green values is at most 8 below and 7 above
				//		the difference between the previous and current green value (dg)
				// Ex.
				//		dg = 24
				//		dr = 20 (dr - dg = -4)
				//		db = 31 (db - dg = 7)
				dg = withinWrappedRange(prevPixel.g, currentPixel.g, 32, 31);
				// Max value is prev + (dg + 7)
				// Min value is prev + (dg - 8)
				dr = withinWrappedRange(prevPixel.r + dg, currentPixel.r, 8, 7);
				db = withinWrappedRange(prevPixel.b + dg, currentPixel.b, 8, 7);

				if (dg != INT_MIN && dr != INT_MIN && db != INT_MIN)
				{
					// OP_LUMA
					// If the OP_LUMA criteria are met, it will be stored in 2 bytes.
					// 2 bit for the tag (0b10), 6 bits for dg (with an offset of 32)
					// 4 bits for both dr and dg (both with an offset of 8). The dr bits are
					// shifted 4 to the left to be saved within the same bit as db.
					outputImage->data[dataIndex] = 0b10000000 | (dg + 32);
					outputImage->data[dataIndex + 1] = (dr + 8) << 4 | (db + 8);
					dataIndex += 2;
				}
				// No other method could save space, however the alpha value is the same as the previous pixel.
				else
				{
					// OP_RGB
					// Save the full byte tag
					outputImage->data[dataIndex] = 0xFE;
					// Save 1 byte per r, g, b value.
					// This loses space over saving the raw pixel data,
					// however hopefully it doesn't need to be done many times.
					outputImage->data[dataIndex + 1] = currentPixel.r;
					outputImage->data[dataIndex + 2] = currentPixel.g;
					outputImage->data[dataIndex + 3] = currentPixel.b;
					dataIndex += 4;
				}
			}
		}
		// The alpha is different and OP_RUN and OP_INDEX were nor applicable.
		else
		{
			// OP_RGBA
			// Save the full byte tag
			outputImage->data[dataIndex] = 0xFF;
			// Save one byte per r, g, b, a value
			// Similarly to OP_RGB, this loses data compared to saving raw pixel data due to the tag.
			// This shouldn't happen regularly though, as changes in alpha are rare.
			outputImage->data[dataIndex + 1] = currentPixel.r;
			outputImage->data[dataIndex + 2] = currentPixel.g;
			outputImage->data[dataIndex + 3] = currentPixel.b;
			outputImage->data[dataIndex + 4] = currentPixel.a;
			dataIndex += 5;
		}

		// Set the new previous pixel to the current pixel.
		prevPixel = currentPixel;
		// Save the current pixel at the corresponding index on the running array.
		runningArray[QOIHash] = currentPixel;
	}
	// If the image ends on a run, the run must be added to the end of the file.
	if (run > 0)
	{
		// Save Run
		saveRun(outputImage->data, &run, &dataIndex);
	}

	// 8 Byte footer for all QOI files. (7 0x00s followed by a 0x01)
	for (int i = 0; i < 7; i++)
	{
		outputImage->data[dataIndex] = 0x00;
		dataIndex++;
	}
	outputImage->data[dataIndex] = 0x01;
	dataIndex++;

	// Set the dataSize of the output image.
	outputImage->dataSize = dataIndex;
}

void importImage(char *fileLocation, struct InputImage *inputImage)
{
	// Predefine the values to be set by the stb_image import (https://github.com/nothings/stb).
	int x, y, n;

	int channels = 4;

	// Use the stb_image library (https://github.com/nothings/stb) to load images of many types.
	// Returns a one dimensional array of pixel values.
	// Each pixel is 4 values in the array (r,g,b,a) and the array length is pixels * 4.
	unsigned char *data = stbi_load(fileLocation, &x, &y, &n, channels);

	// String for file location has to be preallocated.
	inputImage->fileLocation = malloc(sizeof(char) * 261);
	strcpy(inputImage->fileLocation, fileLocation);

	inputImage->width = x;
	inputImage->height = y;
	// Preallocate the size of all the pixels to the array.
	inputImage->pixels = malloc(sizeof(struct Pixel) * x * y);

	// For each pixel, save each channel
	for (int i = 0; i < x * y; i++)
	{
		// i * channels because each pixel takes up that number of array slots.
		inputImage->pixels[i].r = data[i * channels + 0];
		inputImage->pixels[i].g = data[i * channels + 1];
		inputImage->pixels[i].b = data[i * channels + 2];
		inputImage->pixels[i].a = data[i * channels + 3];
	}

	// Free up image memory.
	stbi_image_free(data);
}

void exportQOI(char *fileLocation, struct OutputImage *outputImage)
{
	// Open file in writing, binary mode.
	FILE *f = fopen(fileLocation, "wb");

	// Write all the data stored in the output image.
	// Provide that there are data size * size of char bytes to write.
	fwrite(outputImage->data, sizeof(char), outputImage->dataSize, f);

	fclose(f);
}

char *getLocation(bool import)
{
	// Loop until information that is required has been provided.
	while (true)
	{
		// Preallocate the space for the response.
		char *location = malloc(sizeof(char) * 261);

		// Different messages based on the required file.
		if (import)
		{
			printf("Please enter the location of the file you would like to convert:\n");
		}
		else
		{
			printf("Please enter the location you wish to save as:\n");
		}
		// Get the string from the user and copy it to the memory allocated to location.
		scanf("%s", location);

		// If an import file is being gathered, check it exists.
		// Allow through if not import file.
		if (access(location, F_OK) != -1 || !import)
		{
			// Print the result text and return the location.
			if (import)
			{
				printf("Imported");
			}
			else
			{
				printf("Exported");
			}
			printf(" file\n");
			waitForInput();
			return location;
		}
		else
		{
			// File not found, restart the loop.
			printf("File not found. Try Again\n");
			waitForInput();
		}
	}
}

// Reads the provided args and returns a code based on result.
// -1 = Source File Does Not Exist
// 0 = Incorrect Format
// 1 = Success
int readArgs(int argc, char *argv[], char *importLocation, char *exportLocation)
{
	// Should be 5 args for a correctly formatted request.
	// exec, tag 1, path, tag 2, path
	if (argc != 5)
	{
		// Not enough args.
		return 0;
	}
	else
	{
		// Arg 0 is executable.
		// Arg 1 & 3 should be the parameter tags
		// Arg 2 & 4 should be the file paths.

		if ((strcmp(argv[1], "-s") == 0 || strcmp(argv[1], "--source") == 0) && (strcmp(argv[3], "-d") == 0 || strcmp(argv[3], "--destination") == 0))
		{
			// Arg 2 is source path.
			// Arg 4 is destination path.
			strcpy(importLocation, argv[2]);
			strcpy(exportLocation, argv[4]);
		}
		else if ((strcmp(argv[1], "-d") == 0 || strcmp(argv[1], "--destination") == 0) && (strcmp(argv[3], "-s") == 0 || strcmp(argv[3], "--source") == 0))
		{
			// Arg 2 is destination path.
			// Arg 4 is source path.
			strcpy(exportLocation, argv[2]);
			strcpy(importLocation, argv[4]);
		}
		else
		{
			// Incorrect Format.
			return 0;
		}

		// The access function determines if there is a file at the location.
		// Check if it returns -1, if it does, return -1 (Error code for missing source)
		// and if it doesn't, return success.
		return access(importLocation, F_OK) == -1 ? -1 : 1;
	}
}

void startMenu()
{
	printf("Welcome to Encode QOI.\n");

	// Preallocate the maximum space for the location string.
	char *importLocation = malloc(sizeof(char) * 261);
	// Copy the value of getLocation to the memory allocated previously.
	strcpy(importLocation, getLocation(true));

	struct InputImage inputImage;
	// Import the image located at the file location inputted previously.
	importImage(importLocation, &inputImage);

	// Creates an empty output image to be filled.
	struct OutputImage outputImage;
	// Converts the input image into an output image.
	// Passing both input and output image by reference
	convertToQOI(&inputImage, &outputImage);

	// Free up input image memory.
	free(inputImage.pixels);
	free(inputImage.fileLocation);

	// Get the intended location for the export.
	// Must allocate memory space first.
	char *exportLocation = malloc(sizeof(char) * 261);
	strcpy(exportLocation, getLocation(false));

	// Export the image to the given location.
	exportQOI(exportLocation, &outputImage);

	// Free up the allocated memory.
	free(outputImage.data);
	free(outputImage.fileLocation);
	free(exportLocation);
	free(importLocation);
}

void startCommandLine(int argc, char *argv[])
{
	// Preallocate the maximum space for the location strings.
	char *importLocation = malloc(sizeof(char) * 261);
	char *exportLocation = malloc(sizeof(char) * 261);

	int argResult = readArgs(argc, argv, importLocation, exportLocation);

	if (argResult == 1)
	{
		// Similar to the menu script but doesn't have steps in between to get other information.

		struct InputImage inputImage;
		// Import the image located at the file location inputted previously.
		importImage(importLocation, &inputImage);

		// Creates an empty output image to be filled.
		struct OutputImage outputImage;
		// Converts the input image into an output image.
		// Passing both input and output image by reference
		convertToQOI(&inputImage, &outputImage);

		// Export the image to the given location.
		exportQOI(exportLocation, &outputImage);

		// Free up input image memory.
		free(inputImage.pixels);
		free(inputImage.fileLocation);
		free(outputImage.data);
		free(outputImage.fileLocation);
	}
	else if (argResult == -1)
	{
		printf("Source file does not exist.\n");
	}
	else if (argResult == 0)
	{
		// Help Menu
		// A modular approach that allows for setting commands and automatically populating the
		// help menu is more effort than is needed as there are very few commands. For a project with
		// a larger scope, this would be very helpful.
		printf("Encode QOI.\n");
		printf("Reads images in other formats and encodes them with QOI.\n");
		printf("QOI specifications are available at https://qoiformat.org/\n");
		printf("\n");
		printf("Usage:\n");
		printf("  encodeQOI\n");
		printf("Options:\n");
		printf("  -h --help\t\t\t\t\tShow this screen.\n");
		printf("  (-s | --source) <source file>\t\t\tSet the source file\n");
		printf("  (-d | --destination) <destination file>\tSet the destination file\n");
	}
	// Free up the allocated memory.
	free(exportLocation);
	free(importLocation);
}

int main(int argc, char *argv[])
{
	// 1 arg is always used as the executable.
	if (argc > 1)
	{
		startCommandLine(argc, argv);
	}
	else
	{
		startMenu();
	}

	return 0;
}