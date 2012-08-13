/* reference: http://www.drdobbs.com/database/algorithm-alley/184410326
 */

// imaadpcm.h
#ifndef IMAADPCM_H_INCLUDED
#define IMAADPCM_H_INCLUDED

struct ImaState {
   int index;    // Index into step size table
   int previousValue; // Most recent sample value
};

// Decode/Encode a single sample and update state
short ImaAdpcmDecode(unsigned char deltaCode, ImaState &);
unsigned char ImaAdpcmEncode(short sample, ImaState &);

class DecompressImaAdpcmMs {
private:
   int  _channels;
   short *_samples[2];  // Left and right sample buffers
   short *_samplePtr[2]; // Pointers to current samples
   size_t   _samplesRemaining; // Samples remaining in each channel
   size_t   _samplesPerPacket; // Total samples per packet
public:
   DecompressImaAdpcmMs(int packetLength, int channels);
   ~DecompressImaAdpcmMs();
   size_t GetSamples(short *outBuff, size_t len);
private:
   unsigned char *_packet;   // Temporary buffer for packets
   size_t   _bytesPerPacket; // Size of a packet
   size_t  NextPacket();
   size_t ReadBytes(void *buffer, size_t request);
};

class DecompressImaAdpcmApple {
private:
   int _channels;

   ImaState _state[2];
   short _samples[2][64];
   short *_samplePtr[2];

   size_t   _samplesRemaining;
   size_t  NextPacket(short *sampleBuffer, ImaState &state);

public:
   DecompressImaAdpcmApple(int channels);
   size_t GetSamples(short *outBuff, size_t len);
   size_t ReadBytes(void *buffer, size_t request);
};

#endif
