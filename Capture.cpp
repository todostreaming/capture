/* -LICENSE-START-
** Copyright (c) 2013 Blackmagic Design
**
** Permission is hereby granted, free of charge, to any person or organization
** obtaining a copy of the software and accompanying documentation covered by
** this license (the "Software") to use, reproduce, display, distribute,
** execute, and transmit the Software, and to prepare derivative works of the
** Software, and to permit third-parties to whom the Software is furnished to
** do so, all subject to the following:
**
** The copyright notices in the Software and this entire statement, including
** the above license grant, this restriction and the following disclaimer,
** must be included in all copies of the Software, in whole or in part, and
** all derivative works of the Software, unless such copies or derivative
** works are solely in the form of machine-executable object code generated by
** a source language processor.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
** FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
** SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
** FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
** ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
** DEALINGS IN THE SOFTWARE.
** -LICENSE-END-
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <csignal>
#include <iostream>

#include "DeckLinkAPI.h"
#include "Capture.h"
#include "Config.h"

static pthread_mutex_t	g_sleepMutex;
static pthread_cond_t	g_sleepCond;
static bool				g_do_exit = false;

static BMDConfig		g_config;

static IDeckLinkInput*	g_deckLinkInput = NULL;
static IDeckLink*		deckLink = NULL;
static FILE 			*fp_audio = NULL;
static FILE 			*fp_video = NULL;

static unsigned long	g_frameCount = 0;
static BMDTimeValue 	frameDuration = 0;


void closefiles(){

	if (fp_audio != NULL) fclose(fp_audio);
    if (fp_video != NULL) fclose(fp_video);

}

void alarm_handler (int sig)
{
    std::cerr << "\nTimeout overcome !!!!\n";
    std::cerr << "Badly Exiting ....\n";
    //closefiles();
    exit (1);
}

void cleanup ()
{
    HRESULT result;

    std::cerr << "Entering cleanup()...\n";

    // Release resources when we've finished
    // with it to prevent leaks
    if (g_deckLinkInput != NULL)
    {
        std::cerr << "releasing IDeckLinkInput...\n";
        g_deckLinkInput->Release();
        g_deckLinkInput = NULL;
        std::cerr << "released IDeckLinkInput.\n";
    }

    if (deckLink != NULL)
    {
        std::cerr << "releasing IDeckLink instance...\n";
        deckLink->Release ();
        deckLink = NULL;
        std::cerr << "released IDeckLink instance.\n";
    }

    closefiles();
    std::cerr << "Done with cleanup()\n";
    std::cerr << "Fine Exiting ....\n";
}

DeckLinkCaptureDelegate::DeckLinkCaptureDelegate() : m_refCount(0)
{
	pthread_mutex_init(&m_mutex, NULL);
}

DeckLinkCaptureDelegate::~DeckLinkCaptureDelegate()
{
	pthread_mutex_destroy(&m_mutex);
}

ULONG DeckLinkCaptureDelegate::AddRef(void)
{
	pthread_mutex_lock(&m_mutex);
	m_refCount++;
	pthread_mutex_unlock(&m_mutex);

	return (ULONG)m_refCount;
}

ULONG DeckLinkCaptureDelegate::Release(void)
{
	pthread_mutex_lock(&m_mutex);
	m_refCount--;
	pthread_mutex_unlock(&m_mutex);

	if (m_refCount == 0)
	{
		delete this;
		return 0;
	}

	return (ULONG)m_refCount;
}

HRESULT DeckLinkCaptureDelegate::VideoInputFrameArrived(IDeckLinkVideoInputFrame* videoFrame, IDeckLinkAudioInputPacket* audioFrame)
{
	IDeckLinkVideoFrame*				rightEyeFrame = NULL;
	IDeckLinkVideoFrame3DExtensions*	threeDExtensions = NULL;
	void*								frameBytes;
	void*								audioFrameBytes;

    bool video_signal;

    BMDTimeScale    timeScale = 1000; // milliseconds (ticks per second)
    BMDTimeValue    frameTime = 0;
    BMDTimeValue    packetTime = 0;
    static int64_t  video_pts, audio_pts;
    static int64_t	videoframe_cont = 0, audioframe_cont = 0;

	// Handle Video Frame
	if (videoFrame)
	{
		videoFrame->AddRef();
		videoFrame->GetStreamTime(&frameTime, &frameDuration, timeScale);
		video_pts = frameTime;
		videoframe_cont++;

        if (fp_video == NULL)
        {
            if (!(fp_video = fopen (g_config.m_videoOutputFile, "w+")))
            {
                std::cerr << "Error: could not open video stream file '"
                            << g_config.m_videoOutputFile << "': "
                            << "\n";
        		cleanup();
                exit (1);
            }
        }
		// If 3D mode is enabled we retreive the 3D extensions interface which gives.
		// us access to the right eye frame by calling GetFrameForRightEye() .
		if ( (videoFrame->QueryInterface(IID_IDeckLinkVideoFrame3DExtensions, (void **) &threeDExtensions) != S_OK) ||
			(threeDExtensions->GetFrameForRightEye(&rightEyeFrame) != S_OK))
		{
			rightEyeFrame = NULL;
		}

		if (threeDExtensions)
			threeDExtensions->Release();

		if (videoFrame->GetFlags() & bmdFrameHasNoInputSource)
		{
			video_signal = false;
			fprintf(stderr,"Video captured (#%lu) - No input signal detected\n", g_frameCount);
		}else{
			video_signal = true;
		}
		const char *timecodeString = NULL;
		if (g_config.m_timecodeFormat != 0)
		{
			IDeckLinkTimecode *timecode;
			if (videoFrame->GetTimecode(g_config.m_timecodeFormat, &timecode) == S_OK)
			{
				timecode->GetString(&timecodeString);
			}
		}
	    if (g_frameCount % 10 == 0 && video_signal){
			fprintf(stderr,"Video captured (#%lu) - %s - Size: %li bytes\n",
				g_frameCount,
				rightEyeFrame != NULL ? "Valid Frame (3D left/right)" : "Valid Frame",
				videoFrame->GetRowBytes() * videoFrame->GetHeight());
	    }
	    if (timecodeString) free((void*)timecodeString);

		videoFrame->GetBytes(&frameBytes);
		fwrite (frameBytes, 1, videoFrame->GetRowBytes() * videoFrame->GetHeight(), fp_video);
		fflush (fp_video);

		if (rightEyeFrame)
		{
			rightEyeFrame->GetBytes(&frameBytes);
			fwrite (frameBytes, 1, videoFrame->GetRowBytes() * videoFrame->GetHeight(), fp_video);
			fflush (fp_video);
		}

		if (rightEyeFrame)
			rightEyeFrame->Release();

		g_frameCount++;
	}

	// Handle Audio Frame
	if (audioFrame)
	{
		audioFrame->GetPacketTime(&packetTime, timeScale);
		audio_pts = packetTime;
		audioframe_cont++;

        if (fp_audio == NULL)
        {
			if (!(fp_audio = fopen (g_config.m_audioOutputFile, "w+")))
			{
				std::cerr << "Error: could not open audio stream file '"
							<< g_config.m_audioOutputFile << "': "
							<< "\n";
				cleanup();
				exit (1);
			}
        }
		audioFrame->GetBytes(&audioFrameBytes);
        fwrite (audioFrameBytes, 1, audioFrame->GetSampleFrameCount() * g_config.m_audioChannels * (g_config.m_audioSampleDepth / 8), fp_audio);
        fflush (fp_audio);

	}

	if (videoFrame) videoFrame->Release();

	fprintf(stderr, "A-VPTS(ms)=%ld A-VDelay(ms)=%ld\n", audio_pts - video_pts, (audioframe_cont - videoframe_cont) * frameDuration); // A/V sync < 200 ms plz !!!
	if (abs(audio_pts - video_pts) > g_config.m_avdelay) {
		fprintf(stderr, "AV sync > %d\n", g_config.m_avdelay);
		//g_do_exit = true;
		//pthread_cond_signal(&g_sleepCond);
	}

	if (g_config.m_maxFrames > 0 && videoFrame && g_frameCount >= g_config.m_maxFrames)
	{
		g_do_exit = true;
		pthread_cond_signal(&g_sleepCond);
	}

	return S_OK;
}

HRESULT DeckLinkCaptureDelegate::VideoInputFormatChanged(BMDVideoInputFormatChangedEvents events, IDeckLinkDisplayMode *mode, BMDDetectedVideoInputFormatFlags formatFlags)
{
	return S_OK;
}

static void sigfunc(int signum)
{
	if (signum == SIGINT || signum == SIGTERM) // Ctrl-c and usual kill (15)
		g_do_exit = true;

	pthread_cond_signal(&g_sleepCond);
}

int main(int argc, char *argv[])
{
	HRESULT							result;
	int								exitStatus = 1;
	int								idx;

	IDeckLinkConfiguration *deckLinkConfiguration = NULL;


	IDeckLinkIterator*				deckLinkIterator = NULL;

	IDeckLinkAttributes*			deckLinkAttributes = NULL;
	bool							formatDetectionSupported;

	IDeckLinkDisplayModeIterator*	displayModeIterator = NULL;
	IDeckLinkDisplayMode*			displayMode = NULL;
	char*							displayModeName = NULL;
	BMDDisplayModeSupport			displayModeSupported;

	DeckLinkCaptureDelegate*		delegate = NULL;

	pthread_mutex_init(&g_sleepMutex, NULL);
	pthread_cond_init(&g_sleepCond, NULL);

	signal(SIGINT, sigfunc); // Ctrl-C
	signal(SIGTERM, sigfunc); // usual kill (15)
	signal(SIGHUP, sigfunc); // reload hanged

	// Process the command line arguments
	if (!g_config.ParseArguments(argc, argv))
	{
		g_config.DisplayUsage(exitStatus);
		goto bail;
	}

	// Get the DeckLink device
	deckLinkIterator = CreateDeckLinkIteratorInstance();
	if (!deckLinkIterator)
	{
		fprintf(stderr, "This application requires the DeckLink drivers installed.\n");
		goto bail;
	}

	idx = g_config.m_deckLinkIndex;

	while ((result = deckLinkIterator->Next(&deckLink)) == S_OK)
	{
		if (idx == 0)
			break;
		--idx;

		deckLink->Release();
	}

	if (result != S_OK || deckLink == NULL)
	{
		fprintf(stderr, "Unable to get DeckLink device %u\n", g_config.m_deckLinkIndex);
		goto bail;
	}

	// Get the input (capture) interface of the DeckLink device
	result = deckLink->QueryInterface(IID_IDeckLinkInput, (void**)&g_deckLinkInput);
	if (result != S_OK)
		goto bail;

	// Get the display mode
	if (g_config.m_displayModeIndex == -1)
	{
		// Check the card supports format detection
		result = deckLink->QueryInterface(IID_IDeckLinkAttributes, (void**)&deckLinkAttributes);
		if (result == S_OK)
		{
			result = deckLinkAttributes->GetFlag(BMDDeckLinkSupportsInputFormatDetection, &formatDetectionSupported);
			if (result != S_OK || !formatDetectionSupported)
			{
				fprintf(stderr, "Format detection is not supported on this device\n");
				goto bail;
			}
		}

		g_config.m_inputFlags |= bmdVideoInputEnableFormatDetection;

		// Format detection still needs a valid mode to start with
		idx = 0;
	}
	else
	{
		idx = g_config.m_displayModeIndex;
	}

	result = g_deckLinkInput->GetDisplayModeIterator(&displayModeIterator);
	if (result != S_OK)
		goto bail;

	while ((result = displayModeIterator->Next(&displayMode)) == S_OK)
	{
		if (idx == 0)
			break;
		--idx;

		displayMode->Release();
	}

	if (result != S_OK || displayMode == NULL)
	{
		fprintf(stderr, "Unable to get display mode %d\n", g_config.m_displayModeIndex);
		goto bail;
	}

	// Get display mode name
	result = displayMode->GetName((const char**)&displayModeName);
	if (result != S_OK)
	{
		displayModeName = (char *)malloc(32);
		snprintf(displayModeName, 32, "[index %d]", g_config.m_displayModeIndex);
	}

	// Check display mode is supported with given options
	result = g_deckLinkInput->DoesSupportVideoMode(displayMode->GetDisplayMode(), g_config.m_pixelFormat, bmdVideoInputFlagDefault, &displayModeSupported, NULL);
	if (result != S_OK)
		goto bail;

	if (displayModeSupported == bmdDisplayModeNotSupported)
	{
		fprintf(stderr, "The display mode %s is not supported with the selected pixel format\n", displayModeName);
		goto bail;
	}

	if (g_config.m_inputFlags & bmdVideoInputDualStream3D)
	{
		if (!(displayMode->GetFlags() & bmdDisplayModeSupports3D))
		{
			fprintf(stderr, "The display mode %s is not supported with 3D\n", displayModeName);
			goto bail;
		}
	}

	// Print the selected configuration
	g_config.DisplayConfiguration();

	// Configure the capture callback
	delegate = new DeckLinkCaptureDelegate();
	g_deckLinkInput->SetCallback(delegate);

	// ONLY .TSTS =======================================================>>>>>>
    // Query the DeckLink for its configuration interface
    result = g_deckLinkInput->QueryInterface (IID_IDeckLinkConfiguration,
                        (void **) &deckLinkConfiguration);
    if (result != S_OK)
    {
    	g_deckLinkInput->Release ();
        displayMode->Release ();
        fprintf(stderr, "Could not obtain the IDeckLinkConfiguration interface  \n ");
        exit (1);
    }
    BMDVideoConnection vinput;
    switch (g_config.m_video_input_num)
    {
		case 1:
			vinput = bmdVideoConnectionComposite; fprintf(stderr, "Video Input Selected = Composite  \n ");
			break;
		case 2:
			vinput = bmdVideoConnectionComponent; fprintf(stderr, "Video Input Selected = Components  \n ");
			break;
        case 3:
            vinput = bmdVideoConnectionHDMI; fprintf(stderr, "Video Input Selected = HDMI  \n ");
            break;
        case 4:
            vinput = bmdVideoConnectionSDI; fprintf(stderr, "Video Input Selected = SDI  \n ");
            break;
        case 5:
            vinput = bmdVideoConnectionOpticalSDI; fprintf(stderr, "Video Input Selected = Optical SDI  \n ");
            break;
        case 6:
            vinput = bmdVideoConnectionSVideo; fprintf(stderr, "Video Input Selected = S-Video  \n ");
            break;
        default:
            break;
    }

    result = deckLinkConfiguration->SetInt(bmdDeckLinkConfigVideoInputConnection, vinput );
    if (result != S_OK)
    {
        fprintf(stderr, "Error: could not set video input  \n ");
        exit (1);
    }
    BMDVideoConnection ainput;
    switch (g_config.m_audio_input_num)
    {
		case 1:
			ainput = bmdAudioConnectionAnalog; fprintf(stderr, "Audio Input Selected = Analog  \n ");
			break;
        case 2:
            ainput = bmdAudioConnectionEmbedded; fprintf(stderr, "Audio Input Selected = Embedded (HDMI/SDI)  \n ");
            break;
        case 3:
            ainput = bmdAudioConnectionAESEBU; fprintf(stderr, "Audio Input Selected = AES/EBU  \n ");
            break;
        default:
            break;
    }

    result = deckLinkConfiguration->SetInt(bmdDeckLinkConfigAudioInputConnection, ainput);
    if (result != S_OK)
    {
        fprintf(stderr, "Error: could not set audio input  \n ");
        exit (1);
    }
	// ONLY .TSTS ======== END ========================================>>>>>>

	// Start capture configuration
	result = g_deckLinkInput->EnableVideoInput(displayMode->GetDisplayMode(), g_config.m_pixelFormat, g_config.m_inputFlags);
	if (result != S_OK)
	{
		fprintf(stderr, "Failed to enable video input. Is another application using the card?\n");
		goto bail;
	}

	result = g_deckLinkInput->EnableAudioInput(bmdAudioSampleRate48kHz, g_config.m_audioSampleDepth, g_config.m_audioChannels);
	if (result != S_OK)
		goto bail;

	// Block main thread until signal occurs
 	while (!g_do_exit)
	{
		fprintf(stderr, "Starting Capture\n");
	    result = g_deckLinkInput->StartStreams(); // VideoInputFrameArrived, VideoInputFormatChanged will be called as different threads from now on (capturing .....)
		if (result != S_OK)
			goto bail;

		exitStatus = 0;

		pthread_mutex_lock(&g_sleepMutex);
		pthread_cond_wait(&g_sleepCond, &g_sleepMutex); // will wait sleeping there until this happens: pthread_cond_signal(&g_sleepCond);
		pthread_mutex_unlock(&g_sleepMutex);

		fprintf(stderr, "Flushing Capture\n");
		g_deckLinkInput->PauseStreams();
		g_deckLinkInput->FlushStreams(); // ???
	}
	fprintf(stderr, "Stopping Capture\n");
	g_deckLinkInput->StopStreams();
	g_deckLinkInput->DisableAudioInput();
	g_deckLinkInput->DisableVideoInput();

bail:
	if (displayModeName != NULL)
		free(displayModeName);

	if (displayMode != NULL)
		displayMode->Release();

	if (displayModeIterator != NULL)
		displayModeIterator->Release();

	if (g_deckLinkInput != NULL)
	{
		g_deckLinkInput->Release();
		g_deckLinkInput = NULL;
	}

	if (deckLinkAttributes != NULL)
		deckLinkAttributes->Release();

	if (deckLink != NULL)
		deckLink->Release();

	if (deckLinkIterator != NULL)
		deckLinkIterator->Release();

	closefiles();
	fprintf(stderr, "Fine Exiting ...\n");

	return exitStatus;
}
