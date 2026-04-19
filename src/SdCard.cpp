#include <Arduino.h>
#include "settings.h"

#include "SdCard.h"

#include "Common.h"
#include "Led.h"
#include "Log.h"
#include "MemX.h"
#include "System.h"

#include <esp_timer.h>
#include <esp_random.h>

#ifdef SD_MMC_1BIT_MODE
	#include <driver/sdmmc_host.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef SD_MMC_1BIT_MODE
fs::FS gFSystem = (fs::FS) SD_MMC;
#else
SPIClass spiSD(HSPI);
fs::FS gFSystem = (fs::FS) SD;
#endif

namespace {
	constexpr char benchmarkFilePath[] = "/.sdtest.bin";
	constexpr uint32_t benchmarkRandomSeed = 0x51DCA4D3u;
#ifdef BOARD_HAS_PSRAM
	constexpr size_t benchmarkChunkSize = 16 * 1024;
#else
	constexpr size_t benchmarkChunkSize = 4 * 1024;
#endif
#ifdef SD_MMC_1BIT_MODE
	constexpr std::array<uint32_t, sdCardBenchmarkMaxSweepEntries> benchmarkSweepFrequenciesKHz = {
		SDMMC_FREQ_DEFAULT,
		SDMMC_FREQ_26M,
		SDMMC_FREQ_HIGHSPEED,
	};
	constexpr uint32_t sdCardDefaultFrequencyKHz = SDMMC_FREQ_26M;
#endif

	uint32_t benchmarkNextRandom(uint32_t &state) {
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		return state;
	}

	void benchmarkFillBuffer(uint8_t *buffer, size_t len, uint32_t &state) {
		size_t offset = 0;
		while (offset < len) {
			uint32_t value = benchmarkNextRandom(state);
			const size_t bytesToCopy = std::min(len - offset, sizeof(value));
			memcpy(buffer + offset, &value, bytesToCopy);
			offset += bytesToCopy;
		}
	}

	template <typename T>
	void benchmarkSetMessage(T &target, const char *message) {
		snprintf(target.message, sizeof(target.message), "%s", message ? message : "");
	}

	uint32_t benchmarkDurationMs(int64_t durationUs) {
		return static_cast<uint32_t>(std::max<int64_t>(0, durationUs / 1000));
	}

	float benchmarkRateKiBs(uint32_t totalBytes, uint32_t durationMs) {
		if (durationMs == 0) {
			return 0.f;
		}
		return (static_cast<float>(totalBytes) / 1024.f) / (static_cast<float>(durationMs) / 1000.f);
	}

	void benchmarkReportProgress(const SdCardBenchmarkConfig &config, SdCardBenchmarkPhase phase, uint32_t processedBytes, uint32_t totalBytes, uint32_t frequencyKHz) {
		if (config.progressCallback) {
			config.progressCallback(phase, processedBytes, totalBytes, frequencyKHz, config.progressUserData);
		}
	}

	void benchmarkCopyEntryToResult(const SdCardBenchmarkSweepEntry &entry, SdCardBenchmarkResult &result) {
		result.frequencyKHz = entry.frequencyKHz;
		result.writeDurationMs = entry.writeDurationMs;
		result.readDurationMs = entry.readDurationMs;
		result.verifyDurationMs = entry.verifyDurationMs;
		result.writeSpeedKiBs = entry.writeSpeedKiBs;
		result.readSpeedKiBs = entry.readSpeedKiBs;
		result.verifiedReadSpeedKiBs = entry.verifiedReadSpeedKiBs;
		result.writeErrors = entry.writeErrors;
		result.readErrors = entry.readErrors;
		result.verifyErrors = entry.verifyErrors;
	}

#ifdef SD_MMC_1BIT_MODE
	bool benchmarkSetSdCardFrequency(uint32_t frequencyKHz, char *messageBuffer, size_t messageBufferSize) {
		const esp_err_t err = sdmmc_host_set_card_clk(SDMMC_HOST_SLOT_1, frequencyKHz);
		if (err == ESP_OK) {
			return true;
		}

		if (messageBuffer && messageBufferSize > 0) {
			snprintf(messageBuffer, messageBufferSize, "Failed to switch SD card clock to %lu MHz (0x%x).", frequencyKHz / 1000UL, static_cast<unsigned>(err));
		}
		return false;
	}
#endif

	bool benchmarkRunSingle(const SdCardBenchmarkConfig &config, uint32_t frequencyKHz, uint32_t progressBaseBytes, uint32_t progressTotalBytes, SdCardBenchmarkSweepEntry &entry) {
		entry = SdCardBenchmarkSweepEntry{};
		entry.frequencyKHz = frequencyKHz;

		if (gFSystem.exists(benchmarkFilePath) && !gFSystem.remove(benchmarkFilePath)) {
			benchmarkSetMessage(entry, "Failed to delete previous benchmark file.");
			return false;
		}

		uint8_t *writeBuffer = static_cast<uint8_t *>(malloc(benchmarkChunkSize));
		uint8_t *readBuffer = static_cast<uint8_t *>(malloc(benchmarkChunkSize));
		if (!writeBuffer || !readBuffer) {
			free(writeBuffer);
			free(readBuffer);
			benchmarkSetMessage(entry, "Failed to allocate benchmark buffers.");
			return false;
		}

		uint32_t randomState = benchmarkRandomSeed;
		benchmarkFillBuffer(writeBuffer, benchmarkChunkSize, randomState);

		bool cleanupOkay = true;
		bool benchmarkOkay = false;
		File benchmarkFile = gFSystem.open(benchmarkFilePath, FILE_WRITE);
		if (!benchmarkFile) {
			benchmarkSetMessage(entry, "Failed to open benchmark file for writing.");
			goto cleanup;
		}

		benchmarkSetMessage(entry, "Writing benchmark data...");
		benchmarkReportProgress(config, SdCardBenchmarkPhase::Write, progressBaseBytes, progressTotalBytes, frequencyKHz);
		{
			int64_t writeDurationUs = 0;
			uint32_t writtenBytes = 0;

			while (writtenBytes < config.sizeBytes) {
				const size_t bytesThisRound = std::min<size_t>(benchmarkChunkSize, config.sizeBytes - writtenBytes);
				const int64_t writeStartUs = esp_timer_get_time();
				const size_t bytesWritten = benchmarkFile.write(writeBuffer, bytesThisRound);
				writeDurationUs += esp_timer_get_time() - writeStartUs;
				if (bytesWritten != bytesThisRound) {
					entry.writeErrors++;
					benchmarkSetMessage(entry, "Short write while writing benchmark file.");
					benchmarkFile.close();
					goto cleanup;
				}

				writtenBytes += bytesWritten;
				benchmarkReportProgress(config, SdCardBenchmarkPhase::Write, progressBaseBytes + writtenBytes, progressTotalBytes, frequencyKHz);
			}

			const int64_t flushStartUs = esp_timer_get_time();
			benchmarkFile.flush();
			writeDurationUs += esp_timer_get_time() - flushStartUs;
			entry.writeDurationMs = benchmarkDurationMs(writeDurationUs);
			entry.writeSpeedKiBs = benchmarkRateKiBs(config.sizeBytes, entry.writeDurationMs);
		}
		benchmarkFile.close();

		benchmarkFile = gFSystem.open(benchmarkFilePath, FILE_READ);
		if (!benchmarkFile) {
			benchmarkSetMessage(entry, "Failed to open benchmark file for reading.");
			goto cleanup;
		}

		benchmarkSetMessage(entry, "Reading and verifying benchmark data...");
		benchmarkReportProgress(config, SdCardBenchmarkPhase::Read, progressBaseBytes + config.sizeBytes, progressTotalBytes, frequencyKHz);
		{
			int64_t readDurationUs = 0;
			int64_t verifyDurationUs = 0;
			uint32_t readBytesTotal = 0;

			while (readBytesTotal < config.sizeBytes) {
				const size_t bytesThisRound = std::min<size_t>(benchmarkChunkSize, config.sizeBytes - readBytesTotal);
				const int64_t readStartUs = esp_timer_get_time();
				const size_t bytesRead = benchmarkFile.read(readBuffer, bytesThisRound);
				readDurationUs += esp_timer_get_time() - readStartUs;
				if (bytesRead != bytesThisRound) {
					entry.readErrors++;
					benchmarkSetMessage(entry, "Short read while reading benchmark file.");
					benchmarkFile.close();
					goto cleanup;
				}

				const int64_t verifyStartUs = esp_timer_get_time();
				if (memcmp(readBuffer, writeBuffer, bytesThisRound) != 0) {
					verifyDurationUs += esp_timer_get_time() - verifyStartUs;
					entry.verifyErrors++;
					benchmarkSetMessage(entry, "Verification failed while reading benchmark data.");
					benchmarkFile.close();
					goto cleanup;
				}
				verifyDurationUs += esp_timer_get_time() - verifyStartUs;

				readBytesTotal += bytesRead;
				benchmarkReportProgress(config, SdCardBenchmarkPhase::Read, progressBaseBytes + config.sizeBytes + readBytesTotal, progressTotalBytes, frequencyKHz);
			}

			entry.readDurationMs = benchmarkDurationMs(readDurationUs);
			entry.verifyDurationMs = benchmarkDurationMs(verifyDurationUs);
			entry.readSpeedKiBs = benchmarkRateKiBs(config.sizeBytes, entry.readDurationMs);
			entry.verifiedReadSpeedKiBs = benchmarkRateKiBs(config.sizeBytes, entry.readDurationMs + entry.verifyDurationMs);
		}
		benchmarkFile.close();

		entry.success = true;
		benchmarkSetMessage(entry, "Benchmark completed successfully.");
		benchmarkOkay = true;

cleanup:
		if (benchmarkFile) {
			benchmarkFile.close();
		}
		if (gFSystem.exists(benchmarkFilePath) && !gFSystem.remove(benchmarkFilePath)) {
			cleanupOkay = false;
			if (benchmarkOkay) {
				entry.success = false;
				benchmarkSetMessage(entry, "Benchmark file could not be deleted after the test.");
			}
		}
		free(writeBuffer);
		free(readBuffer);
		return benchmarkOkay && cleanupOkay && entry.success;
	}
} // namespace

void SdCard_Init(void) {
#ifdef NO_SDCARD
	// Initialize without any SD card, e.g. for webplayer only
	Log_Println("Init without SD card ", LOGLEVEL_NOTICE);
	return
#endif

	#ifndef SINGLE_SPI_ENABLE
		#ifdef SD_MMC_1BIT_MODE
			pinMode(2, INPUT_PULLUP);
	while (!SD_MMC.begin("/sdcard", true, false, static_cast<int>(sdCardDefaultFrequencyKHz))) {
		#else
			pinMode(SPISD_CS, OUTPUT);
	digitalWrite(SPISD_CS, HIGH);
	spiSD.begin(SPISD_SCK, SPISD_MISO, SPISD_MOSI, SPISD_CS);
	spiSD.setFrequency(1000000);
	while (!SD.begin(SPISD_CS, spiSD)) {
	#endif
#else
	#ifdef SD_MMC_1BIT_MODE
	pinMode(2, INPUT_PULLUP);
	while (!SD_MMC.begin("/sdcard", true, false, static_cast<int>(sdCardDefaultFrequencyKHz))) {
	#else
	while (!SD.begin(SPISD_CS)) {
	#endif
#endif
		Log_Println(unableToMountSd, LOGLEVEL_ERROR);
		delay(500);
#ifdef SHUTDOWN_IF_SD_BOOT_FAILS
		if (millis() >= deepsleepTimeAfterBootFails * 1000) {
			Log_Println(sdBootFailedDeepsleep, LOGLEVEL_ERROR);
			esp_deep_sleep_start();
		}
#endif
	}
}

void SdCard_Exit(void) {
// SD card goto idle mode
#ifdef SINGLE_SPI_ENABLE
	Log_Println("shutdown SD card (SPI)..", LOGLEVEL_NOTICE);
	SD.end();
#endif
#ifdef SD_MMC_1BIT_MODE
	Log_Println("shutdown SD card (SD_MMC)..", LOGLEVEL_NOTICE);
	SD_MMC.end();
#endif
}

sdcard_type_t SdCard_GetType(void) {
	sdcard_type_t cardType;
#ifdef SD_MMC_1BIT_MODE
	Log_Println(sdMountedMmc1BitMode, LOGLEVEL_NOTICE);
	cardType = SD_MMC.cardType();
#else
	Log_Println(sdMountedSpiMode, LOGLEVEL_NOTICE);
	cardType = SD.cardType();
#endif
	return cardType;
}

uint64_t SdCard_GetSize() {
#ifdef SD_MMC_1BIT_MODE
	return SD_MMC.cardSize();
#else
	return SD.cardSize();
#endif
}

uint64_t SdCard_GetFreeSize() {
#ifdef SD_MMC_1BIT_MODE
	return SD_MMC.cardSize() - SD_MMC.usedBytes();
#else
	return SD.cardSize() - SD.usedBytes();
#endif
}

void SdCard_PrintInfo() {
	// show SD card type
	sdcard_type_t cardType = SdCard_GetType();
	const char *type = "UNKNOWN";
	switch (cardType) {
		case CARD_MMC:
			type = "MMC";
			break;

		case CARD_SD:
			type = "SDSC";
			break;

		case CARD_SDHC:
			type = "SDHC";
			break;

		default:
			break;
	}
	Log_Printf(LOGLEVEL_DEBUG, "SD card type: %s", type);
	// show SD card size / free space
	uint64_t cardSize = SdCard_GetSize() / (1024 * 1024);
	uint64_t freeSize = SdCard_GetFreeSize() / (1024 * 1024);
	;
	Log_Printf(LOGLEVEL_NOTICE, sdInfo, cardSize, freeSize);
}

bool SdCard_RunBenchmark(const SdCardBenchmarkConfig &config, SdCardBenchmarkResult &result) {
	result = SdCardBenchmarkResult{};

#ifdef NO_SDCARD
	benchmarkSetMessage(result, "SD card benchmark is not available.");
	return false;
#endif

	if (config.sizeBytes == 0) {
		benchmarkSetMessage(result, "Invalid benchmark size.");
		return false;
	}

#ifdef SD_MMC_1BIT_MODE
	const bool runFrequencySweep = config.runFrequencySweep;
	const size_t sweepCount = runFrequencySweep ? benchmarkSweepFrequenciesKHz.size() : 1;
#else
	const bool runFrequencySweep = false;
	const size_t sweepCount = 1;
#endif
	result.totalBytes = config.sizeBytes * 2 * sweepCount;

	int bestEntryIndex = -1;
	size_t successfulEntryCount = 0;

	for (size_t entryIndex = 0; entryIndex < sweepCount && result.sweepEntryCount < sdCardBenchmarkMaxSweepEntries; ++entryIndex) {
		SdCardBenchmarkSweepEntry &entry = result.sweepEntries[result.sweepEntryCount++];
#ifdef SD_MMC_1BIT_MODE
		const uint32_t frequencyKHz = runFrequencySweep ? benchmarkSweepFrequenciesKHz[entryIndex] : sdCardDefaultFrequencyKHz;
		if (!benchmarkSetSdCardFrequency(frequencyKHz, entry.message, sizeof(entry.message))) {
			entry = SdCardBenchmarkSweepEntry{};
			entry.frequencyKHz = frequencyKHz;
			snprintf(entry.message, sizeof(entry.message), "Failed to switch SD card clock to %lu MHz.", frequencyKHz / 1000UL);
			continue;
		}
#else
		const uint32_t frequencyKHz = 0;
#endif
		if (benchmarkRunSingle(config, frequencyKHz, config.sizeBytes * 2 * entryIndex, result.totalBytes, entry)) {
			++successfulEntryCount;
			if (bestEntryIndex < 0 || entry.verifiedReadSpeedKiBs > result.sweepEntries[bestEntryIndex].verifiedReadSpeedKiBs) {
				bestEntryIndex = static_cast<int>(entryIndex);
			}
		}
	}

#ifdef SD_MMC_1BIT_MODE
	uint32_t restoredFrequencyKHz = sdCardDefaultFrequencyKHz;
	bool restoreOkay = benchmarkSetSdCardFrequency(sdCardDefaultFrequencyKHz, result.message, sizeof(result.message));
	if (!restoreOkay && bestEntryIndex >= 0 && result.sweepEntries[bestEntryIndex].frequencyKHz != sdCardDefaultFrequencyKHz) {
		restoredFrequencyKHz = result.sweepEntries[bestEntryIndex].frequencyKHz;
		restoreOkay = benchmarkSetSdCardFrequency(restoredFrequencyKHz, result.message, sizeof(result.message));
	}
#else
	const bool restoreOkay = true;
#endif

	result.processedBytes = result.totalBytes;
	result.success = successfulEntryCount > 0;

	if (bestEntryIndex >= 0) {
		benchmarkCopyEntryToResult(result.sweepEntries[bestEntryIndex], result);
	}

	if (!restoreOkay) {
		result.success = false;
		benchmarkSetMessage(result, "Benchmark finished, but the SD card clock could not be restored afterwards.");
		return false;
	}

	if (runFrequencySweep) {
		if (bestEntryIndex >= 0) {
#ifdef SD_MMC_1BIT_MODE
			if (restoredFrequencyKHz != sdCardDefaultFrequencyKHz) {
				snprintf(result.message, sizeof(result.message), "Sweep completed. Best verified read result at %lu MHz. Default 26 MHz restore failed, keeping %lu MHz for now.", result.frequencyKHz / 1000UL, restoredFrequencyKHz / 1000UL);
			} else if (successfulEntryCount < sweepCount) {
				snprintf(result.message, sizeof(result.message), "Sweep completed with partial failures. Best verified read result at %lu MHz.", result.frequencyKHz / 1000UL);
			} else {
				snprintf(result.message, sizeof(result.message), "Sweep completed. Best verified read result at %lu MHz.", result.frequencyKHz / 1000UL);
			}
#endif
			return true;
		}

		benchmarkSetMessage(result, "Frequency sweep failed at all tested SD card frequencies.");
		return false;
	}

	if (bestEntryIndex >= 0) {
		benchmarkSetMessage(result, "Benchmark completed successfully.");
		return true;
	}

	benchmarkSetMessage(result, "SD card benchmark failed.");
	return false;
}

// Check if file-type is correct
bool fileValid(const char *_fileItem) {
	// clang-format off
	// all supported extension
	constexpr std::array audioFileSufix = {
		".mp3",
		".aac",
		".m4a",
		".wav",
		".flac",
		".ogg",
		".oga",
		".opus",
		// playlists
		".m3u",
		".m3u8",
		".pls",
		".asx"
	};
	// clang-format on
	constexpr size_t maxExtLen = strlen(*std::max_element(audioFileSufix.begin(), audioFileSufix.end(), [](const char *a, const char *b) {
		return strlen(a) < strlen(b);
	}));

	if (!_fileItem || !strlen(_fileItem)) {
		// invalid entry
		return false;
	}

	// check for streams
	if (strncmp(_fileItem, "http://", strlen("http://")) == 0 || strncmp(_fileItem, "https://", strlen("https://")) == 0) {
		// this is a stream
		return true;
	}

	// check for files which start with "/."
	const char *lastSlashPtr = strrchr(_fileItem, '/');
	if (lastSlashPtr == nullptr) {
		// we have a relative filename without any slashes...
		// set the pointer so that it points to the first character AFTER a +1
		lastSlashPtr = _fileItem - 1;
	}
	if (*(lastSlashPtr + 1) == '.') {
		// we have a hidden file
		// Log_Printf(LOGLEVEL_DEBUG, "File is hidden: %s", _fileItem);
		return false;
	}

	// extract the file extension
	const char *extStartPtr = strrchr(_fileItem, '.');
	if (extStartPtr == nullptr) {
		// no extension found
		// Log_Printf(LOGLEVEL_DEBUG, "File has no extension: %s", _fileItem);
		return false;
	}
	const size_t extLen = strlen(extStartPtr);
	if (extLen > maxExtLen) {
		// extension too long, we do not care anymore
		// Log_Printf(LOGLEVEL_DEBUG, "File not supported (extension to long): %s", _fileItem);
		return false;
	}
	char extBuffer[maxExtLen + 1] = {0};
	memcpy(extBuffer, extStartPtr, extLen);

	// make the extension lower case (without using non standard C functions)
	for (size_t i = 0; i < extLen; i++) {
		extBuffer[i] = tolower(extBuffer[i]);
	}

	// check extension against all supported values
	for (const auto &e : audioFileSufix) {
		if (strcmp(extBuffer, e) == 0) {
			// hit we found the extension
			return true;
		}
	}
	// miss, we did not find the extension
	// Log_Printf(LOGLEVEL_DEBUG, "File not supported: %s", _fileItem);
	return false;
}

// Takes a directory as input and returns a random subdirectory from it
const String SdCard_pickRandomSubdirectory(const char *_directory) {
	// Look if folder requested really exists and is a folder. If not => break.
	File directory = gFSystem.open(_directory);
	if (!directory || !directory.isDirectory()) {
		Log_Printf(LOGLEVEL_ERROR, dirOrFileDoesNotExist, _directory);
		return String();
	}
	Log_Printf(LOGLEVEL_NOTICE, tryToPickRandomDir, _directory);

	// iterate through and count all dirs
	size_t dirCount = 0;
	while (1) {
		bool isDir;
		const String name = directory.getNextFileName(&isDir);
		if (name.isEmpty()) {
			break;
		}
		if (isDir) {
			dirCount++;
		}
	}
	if (!dirCount) {
		// no paths in folder
		return String();
	}

	const uint32_t randomNumber = esp_random() % dirCount;
	directory.rewindDirectory();
	dirCount = 0;
	while (1) {
		bool isDir;
		const String name = directory.getNextFileName(&isDir);
		if (name.isEmpty()) {
			break;
		}
		if (isDir) {
			if (dirCount == randomNumber) {
				return name;
			}
			dirCount++;
		}
	}

	// if we reached here, something went wrong
	return String();
}

static bool SdCard_allocAndSave(Playlist *playlist, const String &s) {
	const size_t len = s.length() + 1;
	char *entry = static_cast<char *>(x_malloc(len));
	if (!entry) {
		// OOM, free playlist and return
		Log_Println(unableToAllocateMemForLinearPlaylist, LOGLEVEL_ERROR);
		freePlaylist(playlist);
		return false;
	}
	s.toCharArray(entry, len);
	playlist->push_back(entry);
	return true;
};

static std::optional<Playlist *> SdCard_ParseM3UPlaylist(File f, bool forceExtended = false) {
	const String line = f.readStringUntil('\n');
	const bool extended = line.startsWith("#EXTM3U") || forceExtended;
	Playlist *playlist = new Playlist();

	// reserve a sane amount of memory to reduce heap fragmentation
	playlist->reserve(64);
	if (extended) {
		// extended m3u file format
		// ignore all lines starting with '#'

		while (f.available()) {
			String line = f.readStringUntil('\n');
			if (!line.startsWith("#")) {
				// this something we have to save
				line.trim();
				// save string
				if (!SdCard_allocAndSave(playlist, line)) {
					return std::nullopt;
				}
			}
		}
		// resize std::vector memory to fit our count
		playlist->shrink_to_fit();
		return playlist;
	}

	// normal m3u is just a bunch of filenames, 1 / line
	f.seek(0);
	while (f.available()) {
		String line = f.readStringUntil('\n');
		// save string
		if (!SdCard_allocAndSave(playlist, line)) {
			return std::nullopt;
		}
	}
	// resize memory to fit our count
	playlist->shrink_to_fit();
	return playlist;
}

/* Puts SD-file(s) or directory into a playlist
	First element of array always contains the number of payload-items. */
std::optional<Playlist *> SdCard_ReturnPlaylist(const char *fileName, const uint32_t _playMode) {
	// Look if file/folder requested really exists. If not => break.
	File fileOrDirectory = gFSystem.open(fileName);
	if (!fileOrDirectory) {
		Log_Printf(LOGLEVEL_ERROR, dirOrFileDoesNotExist, fileName);
		return std::nullopt;
	}

	Log_Printf(LOGLEVEL_DEBUG, freeMemory, ESP.getFreeHeap());

	// Parse m3u-playlist and create linear-playlist out of it
	if (_playMode == LOCAL_M3U) {
		if (!fileOrDirectory.isDirectory() && fileOrDirectory.size() > 0) {
			// function takes care of everything
			return SdCard_ParseM3UPlaylist(fileOrDirectory);
		}
	}

	// if we reach here, this was not a m3u
	Log_Println(playlistGen, LOGLEVEL_NOTICE);
	Playlist *playlist = new Playlist;

	bool recurse = (_playMode == ALL_TRACKS_OF_ALL_SUBDIRS_SORTED || _playMode == ALL_TRACKS_OF_ALL_SUBDIRS_RANDOM);
	size_t hiddenFiles = 0;

	// (recursive) directory scanning function
	std::function<bool(const String &)> scanDir = [&](const String &dirPath) {
		File dir = gFSystem.open(dirPath);
		if (!dir || !dir.isDirectory()) {
			Log_Printf(LOGLEVEL_ERROR, "Cannot open directory %s", dirPath.c_str());
			return false;
		}
		while (true) {
			bool isDir;
			String name = dir.getNextFileName(&isDir);
			if (name.isEmpty()) {
				break;
			}
			if (isDir) {
				if (recurse && !scanDir(name)) {
					return false;
				}
			} else if (fileValid(name.c_str())) {
				if (!SdCard_allocAndSave(playlist, name)) {
					// OOM, function already took care of house cleaning
					return false;
				}
			} else {
				hiddenFiles++;
			}
		}
		return true;
	};

	// File-mode
	if (!fileOrDirectory.isDirectory()) {
		if (!SdCard_allocAndSave(playlist, fileOrDirectory.path())) {
			// OOM, function already took care of house cleaning
			return std::nullopt;
		}
	}
	// Directory-mode (linear-playlist)
	else {
		playlist->reserve(64); // reserve a sane amount of memory to reduce the number of reallocs
		if (!scanDir(fileName)) {
			// OOM, function already took care of house cleaning
			return std::nullopt;
		}
	}

	playlist->shrink_to_fit();

	Log_Printf(LOGLEVEL_NOTICE, numberOfValidFiles, playlist->size());
	Log_Printf(LOGLEVEL_DEBUG, "Hidden files: %u", hiddenFiles);
	return playlist;
}
