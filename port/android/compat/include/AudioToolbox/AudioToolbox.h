/* The CoreAudio output-unit subset MacNativeGames' mixer uses, implemented over AAudio (compat/src/audiotoolbox_aaudio.c):
   one output stream per unit, interleaved float in the stream format the mixer sets, its render callback driven by
   AAudio's data callback. */
#pragma once
#include <stdint.h>
typedef int32_t OSStatus;
typedef uint32_t UInt32;
typedef double Float64;
#define noErr 0
typedef uint32_t AudioUnitRenderActionFlags;
typedef struct { Float64 mSampleTime; uint64_t mHostTime; } AudioTimeStamp;
typedef struct { UInt32 mNumberChannels, mDataByteSize; void *mData; } AudioBuffer;
typedef struct { UInt32 mNumberBuffers; AudioBuffer mBuffers[1]; } AudioBufferList;
typedef OSStatus (*AURenderCallback)(void *ctx, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames, AudioBufferList *io);
typedef struct { AURenderCallback inputProc; void *inputProcRefCon; } AURenderCallbackStruct;
typedef struct { Float64 mSampleRate; UInt32 mFormatID, mFormatFlags, mBytesPerPacket, mFramesPerPacket, mBytesPerFrame,
                 mChannelsPerFrame, mBitsPerChannel, mReserved; } AudioStreamBasicDescription;
typedef struct { UInt32 componentType, componentSubType, componentManufacturer, componentFlags, componentFlagsMask; } AudioComponentDescription;
typedef struct CompatAudioComponent *AudioComponent;
typedef struct CompatAudioUnit *AudioComponentInstance, *AudioUnit;
#define kAudioUnitType_Output 0x61756f75u
#define kAudioUnitSubType_DefaultOutput 0x64656620u
#define kAudioUnitSubType_RemoteIO 0x72696f63u
#define kAudioUnitManufacturer_Apple 0x6170706cu
#define kAudioFormatLinearPCM 0x6c70636du
#define kAudioFormatFlagIsFloat 1u
#define kAudioFormatFlagIsPacked 8u
#define kAudioUnitProperty_StreamFormat 8u
#define kAudioUnitProperty_SetRenderCallback 23u
#define kAudioUnitScope_Input 1u
AudioComponent AudioComponentFindNext(AudioComponent after, const AudioComponentDescription *d);
OSStatus AudioComponentInstanceNew(AudioComponent c, AudioComponentInstance *out);
OSStatus AudioUnitSetProperty(AudioUnit u, UInt32 id, UInt32 scope, UInt32 element, const void *data, UInt32 size);
OSStatus AudioUnitInitialize(AudioUnit u);
OSStatus AudioOutputUnitStart(AudioUnit u);
OSStatus AudioOutputUnitStop(AudioUnit u);
OSStatus AudioUnitUninitialize(AudioUnit u);
OSStatus AudioComponentInstanceDispose(AudioComponentInstance u);
