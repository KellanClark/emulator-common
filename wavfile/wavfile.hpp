
#ifndef WAVFILE_HPP
#define WAVFILE_HPP

#include "types.hpp"

#include <fstream>

template <typename T>
class WavFile {
public:
	~WavFile() {
		close();
	}

	void open(std::string fileName, u32 sampleRate, u32 numChannels) {
		fileStream.open(fileName, std::ios::binary | std::ios::trunc);

		fileData = {};
		frequency = sampleRate;
		channels = numChannels;
	}

	void close() {
		if (!fileStream.is_open()) {
			return;
		}

		// Fill header
		struct __attribute__((__packed__)) {
			char riffStr[4] = {'R', 'I', 'F', 'F'};
			unsigned int fileSize = 0;
			char waveStr[4] = {'W', 'A', 'V', 'E'};
			char fmtStr[4] = {'f', 'm', 't', ' '};
			unsigned int subchunk1Size = 16;
			unsigned short audioFormat = 1; // Uncompressed PCM
			unsigned short numChannels = 2;
			unsigned int sampleRate = 0;
			unsigned int byteRate = 0;
			unsigned short blockAlign = 0;
			unsigned short bitsPerSample = sizeof(T) * 8;
			char dataStr[4] = {'d', 'a', 't', 'a'};
			unsigned int subchunk2Size = 0;
		} headerData;
		headerData.sampleRate = frequency;
		headerData.byteRate = frequency * sizeof(T) * channels;
		headerData.blockAlign = sizeof(T) * channels;
		headerData.subchunk2Size = (fileData.size() * sizeof(i16));
		headerData.fileSize = sizeof(headerData) - 8 + headerData.subchunk2Size;

		// Write everything
		fileStream.write(reinterpret_cast<const char*>(&headerData), sizeof(headerData));
		fileStream.write(reinterpret_cast<const char*>(fileData.data()), fileData.size() * sizeof(i16));

		// Close file
		fileStream.close();
	}

	void write(u8* data, u32 size) {
		fileData.insert(fileData.end(), reinterpret_cast<T*>(data), reinterpret_cast<T*>(data) + size / sizeof(T));
	}

private:
	std::ofstream fileStream;
	std::vector<T> fileData;

	// Info
	u32 frequency;
	u32 channels;
};

#endif