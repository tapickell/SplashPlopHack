//
//  DDAudioPlayer.m
//  DripDrop
//
//  Created by Matt Stanton on 7/14/13.
//  Copyright (c) 2013 Matt Stanton. All rights reserved.
//

#import "DDAudioPlayer.h"

#import "Novocaine.h"
#import "RingBuffer.h"

#import <vector>

@interface DDAudioPlayer () {
    Novocaine* _audioManager;
    RingBuffer* _ringBuffer;
    std::vector<std::vector<float> > _clips;
    std::vector<int> _playing;
}

@end

static const int cliplength = 4410;
static const float thresh = 0.4;

@implementation DDAudioPlayer

- (id) init {
    self = [super init];
    if (self == nil)
    {
        return nil;
    }
    return self;
}

- (void) dealloc {
    delete _ringBuffer;
}

- (void) setupAudio {
    _ringBuffer = new RingBuffer(5120, 2);
    _ringBuffer->Clear();
    _audioManager = [Novocaine audioManager];
    
    __weak typeof(self) weakSelf = self;
    [_audioManager setInputBlock:^(float *newAudio, UInt32 numSamples, UInt32 numChannels) {
        typeof(self) strongSelf = weakSelf;
        RingBuffer* rb = strongSelf->_ringBuffer;
        rb->AddNewInterleavedFloatData(newAudio, numSamples, numChannels);
        if ((rb->NumUnreadFrames() >= cliplength) && (rb->Max() > thresh)) {
            std::vector<float> clip;
            clip.resize(cliplength*numChannels);
            rb->FetchInterleavedData(&(clip[0]), cliplength, numChannels);
//            for (int i = 0; i < 16; i++) {
//                float frac = (1.0+i) * (1.0 / 17.0);
//                clip[2*i] *= frac;
//                clip[2*i+1] *= frac;
//                clip[clip.size() - 2*i - 1] *= frac;
//                clip[clip.size() - 2*i - 2] *= frac;
//            }
            strongSelf->_clips.push_back(clip);
            strongSelf->_playing.push_back(0);
        }
    }];
    
    [_audioManager setOutputBlock:^(float *audioToPlay, UInt32 numSamples, UInt32 numChannels) {
        typeof(self) strongSelf = weakSelf;
        std::vector<int>& playing = strongSelf->_playing;
        std::vector<std::vector<float> >& clips = strongSelf->_clips;
        for (int j = 0; j < numSamples * numChannels; ++j) {
            audioToPlay[j] = 0.0;
        }
        for (int i = 0; i < playing.size(); ++i) {
            for (int j = 0; j < numSamples * numChannels; ++j) {
                if (playing[i] <= 0) {
                    playing[i] = 0; // no time to debug
                    break;
                }
                audioToPlay[j] += (clips[i][clips[i].size() - (playing[i]--)])*0.2;
            }
        }
    }];
    
    [_audioManager play];
}

- (int) numClips {
    return _clips.size();
}

- (void) playClip:(int)clipId {
    if (_playing[clipId] == 0) {
        _playing[clipId] = _clips[clipId].size();
    }
}

@end
