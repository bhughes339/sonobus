/*
  ==============================================================================

   This file is part of the JUCE library.
   Copyright (c) 2022 - Raw Material Software Limited

   JUCE is an open source library subject to commercial or open-source
   licensing.

   By using JUCE, you agree to the terms of both the JUCE 7 End-User License
   Agreement and JUCE Privacy Policy.

   End User License Agreement: www.juce.com/juce-7-licence
   Privacy Policy: www.juce.com/juce-privacy-policy

   Or: You may also use this code under the terms of the GPL v3 (see
   www.gnu.org/licenses).

   JUCE IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL WARRANTIES, WHETHER
   EXPRESSED OR IMPLIED, INCLUDING MERCHANTABILITY AND FITNESS FOR PURPOSE, ARE
   DISCLAIMED.

  ==============================================================================
*/

namespace juce
{

// This is an AudioTransportSource which will own it's assigned source
struct AudioSourceOwningTransportSource final : public AudioTransportSource
{
    AudioSourceOwningTransportSource (PositionableAudioSource* s, double sr)  : source (s)
    {
        AudioTransportSource::setSource (s, 0, nullptr, sr);
    }

    ~AudioSourceOwningTransportSource()
    {
        setSource (nullptr);
    }

private:
    std::unique_ptr<PositionableAudioSource> source;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioSourceOwningTransportSource)
};

//==============================================================================
// An AudioSourcePlayer which will remove itself from the AudioDeviceManager's
// callback list once it finishes playing its source
struct AutoRemovingTransportSource final : public AudioTransportSource,
                                           private Timer
{
    AutoRemovingTransportSource (MixerAudioSource& mixerToUse, AudioTransportSource* ts, bool ownSource,
                                 int samplesPerBlock, double requiredSampleRate)
        : mixer (mixerToUse), transportSource (ts, ownSource)
    {
        jassert (ts != nullptr);

        setSource (transportSource);

        prepareToPlay (samplesPerBlock, requiredSampleRate);
        start();

        mixer.addInputSource (this, true);
        startTimerHz (10);
    }

    ~AutoRemovingTransportSource() override
    {
        setSource (nullptr);
    }

    void timerCallback() override
    {
        if (! transportSource->isPlaying())
            mixer.removeInputSource (this);
    }

private:
    MixerAudioSource& mixer;
    OptionalScopedPointer<AudioTransportSource> transportSource;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AutoRemovingTransportSource)
};

// An AudioSource which simply outputs a buffer
class AudioBufferSource final : public PositionableAudioSource
{
public:
    AudioBufferSource (AudioBuffer<float>* audioBuffer, bool ownBuffer, bool playOnAllChannels)
        : buffer (audioBuffer, ownBuffer),
          playAcrossAllChannels (playOnAllChannels),
          loopLen(buffer->getNumSamples())
    {}

    //==============================================================================
    void setNextReadPosition (int64 newPosition) override
    {
        jassert (newPosition >= 0);

        if (looping)
            newPosition = newPosition % static_cast<int64> (buffer->getNumSamples());

        position = jmin (buffer->getNumSamples(), static_cast<int> (newPosition));
    }

    int64 getNextReadPosition() const override      { return static_cast<int64> (position); }
    int64 getTotalLength() const override           { return static_cast<int64> (buffer->getNumSamples()); }

    bool isLooping() const override                 { return looping; }
    void setLooping (bool shouldLoop) override      { looping = shouldLoop; }

    void setLoopRange (int64 loopStart, int64 loopLength) override {
        loopStartPos = jmax(0, jmin(static_cast<int>(loopStart), static_cast<int>(buffer->getNumSamples()) - 1));
        loopLen =  jmax(1, jmin(static_cast<int>(buffer->getNumSamples()) - loopStartPos, static_cast<int>(loopLength)));
    }
    void getLoopRange(int64 & loopStart, int64 & loopLength) const override {
        loopStart = loopStartPos; loopLength = loopLen;
    }
        
    //==============================================================================
    void prepareToPlay (int, double) override {}
    void releaseResources() override {}

    void getNextAudioBlock (const AudioSourceChannelInfo& bufferToFill) override
    {
        bufferToFill.clearActiveBufferRegion();

        const int bufferSize = buffer->getNumSamples();
        int samplesNeeded = bufferToFill.numSamples;

        while (samplesNeeded > 0) {

            const int samplesToCopy = jmin (looping ? (loopStartPos + loopLen) - position :  bufferSize - position, samplesNeeded);

            if (samplesToCopy > 0)
            {
                int maxInChannels = buffer->getNumChannels();
                int maxOutChannels = bufferToFill.buffer->getNumChannels();
                
                if (! playAcrossAllChannels) {
                    maxOutChannels = jmin (maxOutChannels, maxInChannels);
                }
                
                for (int i = 0; i < maxOutChannels; ++i) {
                    bufferToFill.buffer->copyFrom (i, bufferToFill.startSample, *buffer,
                                                   i % maxInChannels, position, samplesToCopy);
                }

                position += samplesToCopy;
                samplesNeeded -= samplesToCopy;
            }
            else {
                position += samplesNeeded;
                samplesNeeded = 0;
            }

            if (looping) {
                int posdelta = position - (loopStartPos + loopLen);
                if (posdelta >= 0) {
                    position = loopStartPos + posdelta;
                }
            }
            else {
                position += samplesNeeded - samplesToCopy;
                samplesNeeded = 0; // force to be done
            }
        }

    }

private:
    //==============================================================================
    OptionalScopedPointer<AudioBuffer<float>> buffer;
    int position = 0;
    bool looping = false, playAcrossAllChannels;
    int loopStartPos = 0, loopLen;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioBufferSource)
};

SoundPlayer::SoundPlayer()
    : sampleRate (44100.0), bufferSize (512)
{
    formatManager.registerBasicFormats();
    player.setSource (&mixer);
}

SoundPlayer::~SoundPlayer()
{
    mixer.removeAllInputs();
    player.setSource (nullptr);
}

void SoundPlayer::play (const File& file)
{
    if (file.existsAsFile())
        play (formatManager.createReaderFor (file), true);
}

void SoundPlayer::play (const void* resourceData, size_t resourceSize)
{
    if (resourceData != nullptr && resourceSize > 0)
    {
        auto mem = std::make_unique<MemoryInputStream> (resourceData, resourceSize, false);
        play (formatManager.createReaderFor (std::move (mem)), true);
    }
}

void SoundPlayer::play (AudioFormatReader* reader, bool deleteWhenFinished)
{
    if (reader != nullptr)
        play (new AudioFormatReaderSource (reader, deleteWhenFinished), true, reader->sampleRate);
}

void SoundPlayer::play (AudioBuffer<float>* buffer, bool deleteWhenFinished, bool playOnAllOutputChannels)
{
    if (buffer != nullptr)
        play (new AudioBufferSource (buffer, deleteWhenFinished, playOnAllOutputChannels), true);
}

void SoundPlayer::play (PositionableAudioSource* audioSource, bool deleteWhenFinished, double fileSampleRate)
{
    if (audioSource != nullptr)
    {
        AudioTransportSource* transport = dynamic_cast<AudioTransportSource*> (audioSource);

        if (transport == nullptr)
        {
            if (deleteWhenFinished)
            {
                transport = new AudioSourceOwningTransportSource (audioSource, fileSampleRate);
            }
            else
            {
                transport = new AudioTransportSource();
                transport->setSource (audioSource, 0, nullptr, fileSampleRate);
                deleteWhenFinished = true;
            }
        }

        transport->start();
        transport->prepareToPlay (bufferSize, sampleRate);

        new AutoRemovingTransportSource (mixer, transport, deleteWhenFinished, bufferSize, sampleRate);
    }
    else
    {
        if (deleteWhenFinished)
            delete audioSource;
    }
}

void SoundPlayer::playTestSound()
{
    auto soundLength = (int) sampleRate;
    double frequency = 440.0;
    float amplitude = 0.5f;

    auto phasePerSample = MathConstants<double>::twoPi / (sampleRate / frequency);

    auto* newSound = new AudioBuffer<float> (1, soundLength);

    for (int i = 0; i < soundLength; ++i)
        newSound->setSample (0, i, amplitude * (float) std::sin (i * phasePerSample));

    newSound->applyGainRamp (0, 0, soundLength / 10, 0.0f, 1.0f);
    newSound->applyGainRamp (0, soundLength - soundLength / 4, soundLength / 4, 1.0f, 0.0f);

    play (newSound, true, true);
}

//==============================================================================
void SoundPlayer::audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                                    int numInputChannels,
                                                    float* const* outputChannelData,
                                                    int numOutputChannels,
                                                    int numSamples,
                                                    const AudioIODeviceCallbackContext& context)
{
    player.audioDeviceIOCallbackWithContext (inputChannelData, numInputChannels,
                                             outputChannelData, numOutputChannels,
                                             numSamples, context);
}

void SoundPlayer::audioDeviceAboutToStart (AudioIODevice* device)
{
    if (device != nullptr)
    {
        sampleRate = device->getCurrentSampleRate();
        bufferSize = device->getCurrentBufferSizeSamples();
    }

    player.audioDeviceAboutToStart (device);
}

void SoundPlayer::audioDeviceStopped()
{
    player.audioDeviceStopped();
}

void SoundPlayer::audioDeviceError (const String& errorMessage)
{
    player.audioDeviceError (errorMessage);
}

} // namespace juce
