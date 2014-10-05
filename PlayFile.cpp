
#include <AudioToolbox/AudioToolbox.h>
#include "CAXException.h"
#include "CAAudioUnit.h"

void UsageString(int exitCode)
{
    switch (exitCode) {
        case 1:
            printf ("Usage: record <name of file> <duration :sec>\n");
            break;
        case 2:
            printf ("Usage: play <name of file>\n");
            break;
        case 3:
            printf ("Incorrect command\n");
            printf ("Usage: record <name of file> <duration :sec>\n");
            printf ("Usage: play <name of file>\n");
            break;
    }
    exit(exitCode);
}

typedef struct MyRecorder {
    AudioQueueRef				queue;
    
    CFAbsoluteTime				queueStartStopTime;
    AudioFileID					recordFile;
    SInt64						recordPacket; // current packet number in record file
    Boolean						running;
    Boolean						verbose;
} MyRecorder;

// helper functions
double PrepareFileAU (CAAudioUnit &au, AudioStreamBasicDescription &fileFormat, AudioFileID audioFile);
void MakeSimpleGraph (AUGraph &theGraph, CAAudioUnit &fileAU, AudioFileID audioFile);

static void MyInputBufferHandler(	void *                          inUserData,
                                 AudioQueueRef                   inAQ,
                                 AudioQueueBufferRef             inBuffer,
                                 const AudioTimeStamp *          inStartTime,
                                 UInt32							inNumPackets,
                                 const AudioStreamPacketDescription *inPacketDesc)
{
    MyRecorder *aqr = (MyRecorder *)inUserData;
        if (aqr->verbose) {
            printf("buf data %p, 0x%x bytes, 0x%x packets\n", inBuffer->mAudioData,
                   (int)inBuffer->mAudioDataByteSize, (int)inNumPackets);
        }
        
        if (inNumPackets > 0) {
            
            AudioFileWritePackets(aqr->recordFile, FALSE, inBuffer->mAudioDataByteSize,
                                                inPacketDesc, aqr->recordPacket, &inNumPackets, inBuffer->mAudioData);
            aqr->recordPacket += inNumPackets;
        }
        // if we're not stopping, re-enqueue the buffe so that it gets filled again
        if (aqr->running)
            AudioQueueEnqueueBuffer(inAQ, inBuffer, 0, NULL);
}

static void MyPropertyListener(void *userData, AudioQueueRef queue, AudioQueuePropertyID propertyID)
{
    MyRecorder *aqr = (MyRecorder *)userData;
    if (propertyID == kAudioQueueProperty_IsRunning)
        aqr->queueStartStopTime = CFAbsoluteTimeGetCurrent();
}

static Boolean InferAudioFileFormatFromFilename(CFStringRef filename, AudioFileTypeID *outFiletype)
{
    OSStatus err;
    
    // find the extension in the filename.
    CFRange range = CFStringFind(filename, CFSTR("."), kCFCompareBackwards);
    if (range.location == kCFNotFound)
        return FALSE;
    range.location += 1;
    range.length = CFStringGetLength(filename) - range.location;
    CFStringRef extension = CFStringCreateWithSubstring(NULL, filename, range);
    
    UInt32 propertySize = sizeof(AudioFileTypeID);
    err = AudioFileGetGlobalInfo(kAudioFileGlobalInfo_TypesForExtension, sizeof(extension), &extension, &propertySize, outFiletype);
    CFRelease(extension);
    
    return (err == noErr && propertySize > 0);
}

static void MyCopyEncoderCookieToFile(AudioQueueRef theQueue, AudioFileID theFile)
{
    OSStatus err;
    UInt32 propertySize;
    
    // get the magic cookie, if any, from the converter
    err = AudioQueueGetPropertySize(theQueue, kAudioConverterCompressionMagicCookie, &propertySize);
    
    if (err == noErr && propertySize > 0) {
        // there is valid cookie data to be fetched;  get it
        Byte *magicCookie = (Byte *)malloc(propertySize);
        try {
            AudioQueueGetProperty(theQueue, kAudioConverterCompressionMagicCookie, magicCookie,
                                                &propertySize);
            AudioFileSetProperty(theFile, kAudioFilePropertyMagicCookieData, propertySize, magicCookie);
        }
        catch (CAXException e) {
            char buf[256];
            fprintf(stderr, "MyCopyEncoderCookieToFile: %s (%s)\n", e.mOperation, e.FormatError(buf));
        }
        free(magicCookie);
    }
}

static int MyComputeRecordBufferSize(const AudioStreamBasicDescription *format, AudioQueueRef queue, float seconds)
{
    int packets, frames, bytes;
    
    frames = (int)ceil(seconds * format->mSampleRate);
    
    if (format->mBytesPerFrame > 0)
        bytes = frames * format->mBytesPerFrame;
    else {
        UInt32 maxPacketSize;
        if (format->mBytesPerPacket > 0)
            maxPacketSize = format->mBytesPerPacket;	// constant packet size
        else {
            UInt32 propertySize = sizeof(maxPacketSize);
            AudioQueueGetProperty(queue, kAudioConverterPropertyMaximumOutputPacketSize, &maxPacketSize,
                                                &propertySize);
        }
        if (format->mFramesPerPacket > 0)
            packets = frames / format->mFramesPerPacket;
        else
            packets = frames;	// worst-case scenario: 1 frame in a packet
        if (packets == 0)		// sanity check
            packets = 1;
        bytes = packets * maxPacketSize;
    }
    return bytes;
}

int main (int argc, char * const argv[])
{
    if ( argc > 2 ) {
        const char *arg = argv[1];

        const char* suffix = ".wav";
        if (strcmp(arg, "record") == 0 && argc == 4) {
            MyRecorder aqr;
            UInt32 size;
            CFURLRef url;
            const char *recordFileName = NULL;
            OSStatus err = noErr;
            float seconds = 0;
            
            char *argfileName = argv[2];
            if (argfileName == NULL) {
                UsageString(1);
            }
            strcat(argfileName, suffix);
            
            const char *argTime = argv[3];

            if ( sscanf(argTime, "%f", &seconds ) != 1 )
                UsageString(1);
            aqr.verbose = TRUE;
            
            recordFileName = strcat( argv[0], argfileName );
            
            AudioFileTypeID audioFileType = kAudioFileWAVEType;
            CFStringRef cfRecordFileName = CFStringCreateWithCString(NULL, recordFileName, kCFStringEncodingUTF8);
            InferAudioFileFormatFromFilename(cfRecordFileName, &audioFileType);
            CFRelease(cfRecordFileName);
            try {
                AudioStreamBasicDescription df;
                df.mFormatID = kAudioFormatLinearPCM;
                df.mSampleRate = 48000.0;
                df.mChannelsPerFrame = 1; // Mono
                df.mBitsPerChannel = 32;
                df.mBytesPerPacket =
                df.mBytesPerFrame =
                df.mChannelsPerFrame * sizeof(AudioSampleType);
                df.mFramesPerPacket = 1;
                df.mFormatFlags = kAudioFormatFlagsCanonical;
                // create the queue
                AudioQueueNewInput(
                                                 &df,
                                                 MyInputBufferHandler,
                                                 &aqr /* userData */,
                                                 NULL /* run loop */, NULL /* run loop mode */,
                                                 0 /* flags */, &aqr.queue);
                
                // get the record format back from the queue's audio converter --
                // the file may require a more specific stream description than was necessary to create the encoder.
                size = sizeof(df);
                AudioQueueGetProperty(aqr.queue, kAudioConverterCurrentOutputStreamDescription,
                                                    &df, &size);
                
                // convert recordFileName from C string to CFURL
                url = CFURLCreateFromFileSystemRepresentation(NULL, (Byte *)recordFileName, strlen(recordFileName), FALSE);
                
                // create the audio file
                err = AudioFileCreateWithURL(url, audioFileType, &df, kAudioFileFlags_EraseFile, &aqr.recordFile);
                CFRelease(url);
                
                // copy the cookie first to give the file object as much info as we can about the data going in
                MyCopyEncoderCookieToFile(aqr.queue, aqr.recordFile);
                
                // allocate and enqueue buffers
                int bufferByteSize = MyComputeRecordBufferSize(&df, aqr.queue, 0.5);	// enough bytes for half a second
                for (int i = 0; i < 3; ++i) {
                    AudioQueueBufferRef buffer;
                    AudioQueueAllocateBuffer(aqr.queue, bufferByteSize, &buffer);
                    AudioQueueEnqueueBuffer(aqr.queue, buffer, 0, NULL);
                }
                
                // record
                if (seconds > 0) {
                    // user requested a fixed-length recording (specified a duration with -s)
                    // to time the recording more accurately, watch the queue's IsRunning property
                    AudioQueueAddPropertyListener(aqr.queue, kAudioQueueProperty_IsRunning,
                                                                MyPropertyListener, &aqr);
                    
                    // start the queue
                    aqr.running = TRUE;
                    AudioQueueStart(aqr.queue, NULL);
                    CFAbsoluteTime waitUntil = CFAbsoluteTimeGetCurrent() + 10;
                    
                    // wait for the started notification
                    while (aqr.queueStartStopTime == 0.) {
                        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.010, FALSE);
                        if (CFAbsoluteTimeGetCurrent() >= waitUntil) {
                            fprintf(stderr, "Timeout waiting for the queue's IsRunning notification\n");
                            goto cleanup;
                        }
                    }
                    printf("Recording...\n");
                    CFAbsoluteTime stopTime = aqr.queueStartStopTime + seconds;
                    CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
                    CFRunLoopRunInMode(kCFRunLoopDefaultMode, stopTime - now, FALSE);
                } else {
                    // start the queue
                    aqr.running = TRUE;
                    AudioQueueStart(aqr.queue, NULL);
                    
                    // and wait
                    printf("Recording, press <return> to stop:\n");
                    getchar();
                }
                
                // end recording
                printf("* recording done *\n");
                
                aqr.running = FALSE;
                AudioQueueStop(aqr.queue, TRUE);
                
                MyCopyEncoderCookieToFile(aqr.queue, aqr.recordFile);
                
            cleanup:
                AudioQueueDispose(aqr.queue, TRUE);
                AudioFileClose(aqr.recordFile);
            }
            catch (CAXException e) {
                char buf[256];
                fprintf(stderr, "MyInputBufferHandler: %s (%s)\n", e.mOperation, e.FormatError(buf));
                return e.mError;
            }
        }
        if (strcmp(arg, "play") == 0 && argc == 3 ) {
            AudioFileID audioFile;
            
            char* inputFileName = argv[2];
            
            strcat(inputFileName, suffix);
            
            const char* inputFile = NULL;
            inputFile = strcat(argv[0], inputFileName);
            
            CFURLRef theURL = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault, (UInt8*)inputFile, strlen(inputFile), false);
            
            AudioFileOpenURL (theURL, kAudioFileReadPermission, 0, &audioFile);
            CFRelease(theURL);
            AudioStreamBasicDescription	fileFormat = { 0.0, 0, 0, 0, 0, 0, 0, 0, 0 };
            
            UInt32 propsize = sizeof(fileFormat);
            
            OSStatus status = AudioFileGetProperty(audioFile, kAudioFilePropertyDataFormat, &propsize, &fileFormat);
            if (status) {
                UsageString(2);
            }
            // lets set up our playing state now
            AUGraph theGraph;
            CAAudioUnit fileAU;
            
            // this makes the graph, the file AU and sets it all up for playing
            MakeSimpleGraph (theGraph, fileAU, audioFile);
            
            Float64 fileDuration = PrepareFileAU (fileAU, fileFormat, audioFile);
            
            printf ("file duration: %f secs\n", fileDuration);
            printf ("Playing....\n");
            // start playing
            AUGraphStart (theGraph);
            
            
            // sleep until the file is finished
            usleep ((int)(fileDuration * 1000. * 1000.));
        }
        else{
            UsageString(3);
        }
    }
    else
        UsageString(3);
    return 0;
}

double PrepareFileAU (CAAudioUnit &au, AudioStreamBasicDescription &fileFormat, AudioFileID audioFile)
{
    //
    // calculate the duration
    UInt64 nPackets;
    UInt32 propsize = sizeof(nPackets);
    AudioFileGetProperty(audioFile, kAudioFilePropertyAudioDataPacketCount, &propsize, &nPackets);
    
    Float64 fileDuration = (nPackets * fileFormat.mFramesPerPacket) / fileFormat.mSampleRate;
    
    ScheduledAudioFileRegion rgn;
    memset (&rgn.mTimeStamp, 0, sizeof(rgn.mTimeStamp));
    rgn.mTimeStamp.mFlags = kAudioTimeStampSampleTimeValid;
    rgn.mTimeStamp.mSampleTime = 0;
    rgn.mCompletionProc = NULL;
    rgn.mCompletionProcUserData = NULL;
    rgn.mAudioFile = audioFile;
    rgn.mLoopCount = 1;
    rgn.mStartFrame = 0;
    rgn.mFramesToPlay = UInt32(nPackets * fileFormat.mFramesPerPacket);
    
    // tell the file player AU to play all of the file
    au.SetProperty (kAudioUnitProperty_ScheduledFileRegion,
                                   kAudioUnitScope_Global, 0,&rgn, sizeof(rgn));
    
    // prime the fp AU with default values
    UInt32 defaultVal = 0;
    au.SetProperty (kAudioUnitProperty_ScheduledFilePrime,
                                   kAudioUnitScope_Global, 0, &defaultVal, sizeof(defaultVal));
    
    // tell the fp AU when to start playing (this ts is in the AU's render time stamps; -1 means next render cycle)
    AudioTimeStamp startTime;
    memset (&startTime, 0, sizeof(startTime));
    startTime.mFlags = kAudioTimeStampSampleTimeValid;
    startTime.mSampleTime = -1;
    au.SetProperty(kAudioUnitProperty_ScheduleStartTimeStamp,
                                  kAudioUnitScope_Global, 0, &startTime, sizeof(startTime));
    
    return fileDuration;
}



void MakeSimpleGraph (AUGraph &theGraph, CAAudioUnit &fileAU, AudioFileID audioFile)
{
    NewAUGraph (&theGraph);
    
    CAComponentDescription cd;
    
    // output node
    cd.componentType = kAudioUnitType_Output;
    cd.componentSubType = kAudioUnitSubType_DefaultOutput;
    cd.componentManufacturer = kAudioUnitManufacturer_Apple;
    
    AUNode outputNode;
    AUGraphAddNode (theGraph, &cd, &outputNode);
    
    // file AU node
    AUNode fileNode;
    cd.componentType = kAudioUnitType_Generator;
    cd.componentSubType = kAudioUnitSubType_AudioFilePlayer;
    
    AUGraphAddNode (theGraph, &cd, &fileNode);
    
    // connect & setup
    AUGraphOpen (theGraph);
    
    // install overload listener to detect when something is wrong
    AudioUnit anAU;
    AUGraphNodeInfo(theGraph, fileNode, NULL, &anAU);
    //	fileAU = CAAudioUnit (fileNode, anAU);
    fileAU = CAAudioUnit (fileNode, anAU);
    
    // load in the file
    fileAU.SetProperty(kAudioUnitProperty_ScheduledFileIDs,
                                      kAudioUnitScope_Global, 0, &audioFile, sizeof(audioFile));
    
    
    AUGraphConnectNodeInput (theGraph, fileNode, 0, outputNode, 0);
    
    // AT this point we make sure we have the file player AU initialized
    // this also propogates the output format of the AU to the output unit
    AUGraphInitialize (theGraph);
    
    // workaround a race condition in the file player AU
    usleep (10 * 1000);
}