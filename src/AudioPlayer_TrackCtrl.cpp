#include <Arduino.h>
#include "settings.h"

#include "Audio.h"
#include "AudioPlayer.h"
#include "Bluetooth.h"
#include "Cmd.h"
#include "Common.h"
#include "Led.h"
#include "Log.h"
#include "MemX.h"
#include "Mqtt.h"
#include "Queues.h"
#include "SdCard.h"
#include "System.h"
#include "Web.h"

namespace AudioPlayer_TrackCtrl {

void executeTrackCommand(Audio *audio, uint8_t cmd) {
	if (!audio) {
		return;
	}

	switch (cmd) {
		case STOP:
			audio->stopSong();
			Log_Println(cmndStop, LOGLEVEL_INFO);
			gPlayProperties.pausePlay = true;
			gPlayProperties.playlistFinished = true;
			gPlayProperties.playMode = NO_PLAYLIST;
			Audio_setTitle(noPlaylist);
			AudioPlayer_ClearCover();
			break;

		case PAUSEPLAY:
			audio->pauseResume();
			if (gPlayProperties.pausePlay) {
				Log_Println(cmndResumeFromPause, LOGLEVEL_INFO);
			} else {
				Log_Println(cmndPause, LOGLEVEL_INFO);
			}
			if (gPlayProperties.saveLastPlayPosition && !gPlayProperties.pausePlay) {
				Log_Printf(LOGLEVEL_INFO, trackPausedAtPos, audio->getFilePos(), audio->getFilePos() - audio->inBufferFilled());
				AudioPlayer_NvsRfidWriteWrapper(gPlayProperties.playRfidTag, gPlayProperties.playlist->at(gPlayProperties.currentTrackNumber), audio->getFilePos() - audio->inBufferFilled(), gPlayProperties.playMode, gPlayProperties.currentTrackNumber, gPlayProperties.playlist->size());
			}
			gPlayProperties.pausePlay = !gPlayProperties.pausePlay;
			Web_SendWebsocketData(0, WebsocketCodeType::TrackInfo);
			break;

		case NEXTTRACK:
			if (gPlayProperties.pausePlay) {
				audio->pauseResume();
				gPlayProperties.pausePlay = false;
			}
			if (gPlayProperties.repeatCurrentTrack) { // End loop if button was pressed
				gPlayProperties.repeatCurrentTrack = false;
#ifdef MQTT_ENABLE
				publishMqtt(topicRepeatModeState, AudioPlayer_GetRepeatMode(), false);
#endif
			}
			// Allow next track if current track played in playlist isn't the last track.
			// Exception: loop-playlist is active. In this case playback restarts at the first track of the playlist.
			if ((gPlayProperties.currentTrackNumber + 1 < gPlayProperties.playlist->size()) || gPlayProperties.repeatPlaylist) {
				if ((gPlayProperties.currentTrackNumber + 1 >= gPlayProperties.playlist->size()) && gPlayProperties.repeatPlaylist) {
					gPlayProperties.currentTrackNumber = 0;
				} else {
					gPlayProperties.currentTrackNumber++;
				}
				if (gPlayProperties.saveLastPlayPosition) {
					AudioPlayer_NvsRfidWriteWrapper(gPlayProperties.playRfidTag, gPlayProperties.playlist->at(gPlayProperties.currentTrackNumber), 0, gPlayProperties.playMode, gPlayProperties.currentTrackNumber, gPlayProperties.playlist->size());
					Log_Println(trackStartAudiobook, LOGLEVEL_INFO);
				}
				Log_Println(cmndNextTrack, LOGLEVEL_INFO);
				if (!gPlayProperties.playlistFinished) {
					audio->stopSong();
				}
			} else {
				Log_Println(lastTrackAlreadyActive, LOGLEVEL_NOTICE);
				System_IndicateError();
				break;
			}
			break;

		case PREVIOUSTRACK:
			if (gPlayProperties.pausePlay) {
				audio->pauseResume();
				gPlayProperties.pausePlay = false;
			}
			if (gPlayProperties.repeatCurrentTrack) { // End loop if button was pressed
				gPlayProperties.repeatCurrentTrack = false;
#ifdef MQTT_ENABLE
				publishMqtt(topicRepeatModeState, AudioPlayer_GetRepeatMode(), false);
#endif
			}
			if (gPlayProperties.playMode == WEBSTREAM) {
				Log_Println(trackChangeWebstream, LOGLEVEL_INFO);
				System_IndicateError();
				break;
			} else if (gPlayProperties.playMode == LOCAL_M3U) {
				Log_Println(cmndPrevTrack, LOGLEVEL_INFO);
				if (gPlayProperties.currentTrackNumber > 0) {
					gPlayProperties.currentTrackNumber--;
				} else {
					System_IndicateError();
					break;
				}
			} else {
				if (gPlayProperties.currentTrackNumber > 0 || gPlayProperties.repeatPlaylist) {
					if (audio->getAudioCurrentTime() < 5) { // play previous track when current track time is small, else play current track again
						if (gPlayProperties.currentTrackNumber == 0 && gPlayProperties.repeatPlaylist) {
							gPlayProperties.currentTrackNumber = gPlayProperties.playlist->size() - 1; // Go back to last track in loop-mode when first track is played
						} else {
							gPlayProperties.currentTrackNumber--;
						}
					}

					if (gPlayProperties.saveLastPlayPosition) {
						AudioPlayer_NvsRfidWriteWrapper(gPlayProperties.playRfidTag, gPlayProperties.playlist->at(gPlayProperties.currentTrackNumber), 0, gPlayProperties.playMode, gPlayProperties.currentTrackNumber, gPlayProperties.playlist->size());
						Log_Println(trackStartAudiobook, LOGLEVEL_INFO);
					}

					Log_Println(cmndPrevTrack, LOGLEVEL_INFO);
					if (!gPlayProperties.playlistFinished) {
						audio->stopSong();
					}
				} else {
					if (gPlayProperties.saveLastPlayPosition) {
						AudioPlayer_NvsRfidWriteWrapper(gPlayProperties.playRfidTag, gPlayProperties.playlist->at(gPlayProperties.currentTrackNumber), 0, gPlayProperties.playMode, gPlayProperties.currentTrackNumber, gPlayProperties.playlist->size());
					}
					audio->stopSong();
					Led_Indicate(LedIndicatorType::Rewind);
					bool audioReturnCode = audio->connecttoFS(gFSystem, gPlayProperties.playlist->at(gPlayProperties.currentTrackNumber));
					// consider track as finished, when audio lib call was not successful
					if (!audioReturnCode) {
						System_IndicateError();
						gPlayProperties.trackFinished = true;
						break;
					}
					Log_Println(trackStart, LOGLEVEL_INFO);
					break;
				}
			}
			break;
		case FIRSTTRACK:
			if (gPlayProperties.pausePlay) {
				audio->pauseResume();
				gPlayProperties.pausePlay = false;
			}
			gPlayProperties.currentTrackNumber = 0;
			if (gPlayProperties.saveLastPlayPosition) {
				AudioPlayer_NvsRfidWriteWrapper(gPlayProperties.playRfidTag, gPlayProperties.playlist->at(gPlayProperties.currentTrackNumber), 0, gPlayProperties.playMode, gPlayProperties.currentTrackNumber, gPlayProperties.playlist->size());
				Log_Println(trackStartAudiobook, LOGLEVEL_INFO);
			}
			Log_Println(cmndFirstTrack, LOGLEVEL_INFO);
			if (!gPlayProperties.playlistFinished) {
				audio->stopSong();
			}
			break;

		case LASTTRACK:
			if (gPlayProperties.pausePlay) {
				audio->pauseResume();
				gPlayProperties.pausePlay = false;
			}
			if (gPlayProperties.currentTrackNumber + 1 < gPlayProperties.playlist->size()) {
				gPlayProperties.currentTrackNumber = gPlayProperties.playlist->size() - 1;
				if (gPlayProperties.saveLastPlayPosition) {
					AudioPlayer_NvsRfidWriteWrapper(gPlayProperties.playRfidTag, gPlayProperties.playlist->at(gPlayProperties.currentTrackNumber), 0, gPlayProperties.playMode, gPlayProperties.currentTrackNumber, gPlayProperties.playlist->size());
					Log_Println(trackStartAudiobook, LOGLEVEL_INFO);
				}
				Log_Println(cmndLastTrack, LOGLEVEL_INFO);
				if (!gPlayProperties.playlistFinished) {
					audio->stopSong();
				}
			} else {
				Log_Println(lastTrackAlreadyActive, LOGLEVEL_NOTICE);
				System_IndicateError();
				break;
			}
			break;

		case 0:
			break;

		default:
			Log_Println(cmndDoesNotExist, LOGLEVEL_NOTICE);
			System_IndicateError();
			break;
	}
}

} // namespace AudioPlayer_Internal
