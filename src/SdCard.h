#pragma once
#include <Arduino.h>

#include "settings.h"
#ifdef SD_MMC_1BIT_MODE
	#include "SD_MMC.h"
#else
	#include "SD.h"
#endif

extern fs::FS gFSystem;

#include "Playlist.h"

#include <optional>

enum class SdCardBenchmarkPhase : uint8_t {
	Write = 0,
	Read = 1,
};

constexpr size_t sdCardBenchmarkMaxSweepEntries = 3;

typedef void (*SdCardBenchmarkProgressCallback)(SdCardBenchmarkPhase phase, uint32_t processedBytes, uint32_t totalBytes, uint32_t frequencyKHz, void *userData);

typedef struct {
	uint32_t sizeBytes = 0;
	bool runFrequencySweep = false;
	SdCardBenchmarkProgressCallback progressCallback = nullptr;
	void *progressUserData = nullptr;
} SdCardBenchmarkConfig;

typedef struct {
	uint32_t frequencyKHz = 0;
	uint32_t writeDurationMs = 0;
	uint32_t readDurationMs = 0;
	uint32_t verifyDurationMs = 0;
	float writeSpeedKiBs = 0.f;
	float readSpeedKiBs = 0.f;
	float verifiedReadSpeedKiBs = 0.f;
	uint32_t writeErrors = 0;
	uint32_t readErrors = 0;
	uint32_t verifyErrors = 0;
	bool success = false;
	char message[96] = {0};
} SdCardBenchmarkSweepEntry;

typedef struct {
	uint32_t totalBytes = 0;
	uint32_t processedBytes = 0;
	uint32_t frequencyKHz = 0;
	uint32_t writeDurationMs = 0;
	uint32_t readDurationMs = 0;
	uint32_t verifyDurationMs = 0;
	float writeSpeedKiBs = 0.f;
	float readSpeedKiBs = 0.f;
	float verifiedReadSpeedKiBs = 0.f;
	uint8_t sweepEntryCount = 0;
	SdCardBenchmarkSweepEntry sweepEntries[sdCardBenchmarkMaxSweepEntries] = {};
	uint32_t writeErrors = 0;
	uint32_t readErrors = 0;
	uint32_t verifyErrors = 0;
	bool success = false;
	char message[128] = {0};
} SdCardBenchmarkResult;

void SdCard_Init(void);
void SdCard_Exit(void);
sdcard_type_t SdCard_GetType(void);
uint64_t SdCard_GetSize();
uint64_t SdCard_GetFreeSize();
void SdCard_PrintInfo();
bool SdCard_RunBenchmark(const SdCardBenchmarkConfig &config, SdCardBenchmarkResult &result);
std::optional<Playlist *> SdCard_ReturnPlaylist(const char *fileName, const uint32_t _playMode);
const String SdCard_pickRandomSubdirectory(const char *_directory);
