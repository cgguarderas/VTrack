#include "stdafx.h"

#include "VTrackEffect.h"
#include "againcids.h"
#include "againparamids.h"

using namespace Steinberg::Vst;
using namespace Steinberg;
using std::vector;
using std::unique_ptr;
using std::cerr;

#define SAMPLE_RATE 44100
#define NUM_INPUTS 4
#define NUM_OUTPUTS 4
#define NUM_TRACKS 8
#define PATTERN_SCALE 16 // 16 = 16th notes
#define TRIGS_PER_QN (PATTERN_SCALE / 4)
#define PATTERN_LENGTH_QN 4
#define PATTERN_LENGTH (PATTERN_LENGTH_QN * TRIGS_PER_QN)
#define MAX_STACK_SIZE 16 // room for 256 in sample index though

template <typename T, typename U>
void set_bit(T& dst, const U& bit, bool value) {
	if (value) {
		dst |= bit;
	} else {
		dst &= ~bit;
	}
}

double int_param(double value, double max) {
	return min(max, value * (max + 1));
}

enum TrigFlags {
	kTrigEnable = 1 << 0,
};

#if 0
int16 channel;			///< channel index in event bus
int16 pitch;			///< range [0, 127] = [C-2, G8] with A3=440Hz
float tuning;			///< 1.f = +1 cent, -1.f = -1 cent
float velocity;			///< range [0.0, 1.0]
int32 length;           ///< in sample frames (optional, Note Off has to follow in any case!)
int32 noteId;			///< note identifier (if not available then -1)
#endif
enum MidiTrigFlags {
	kMidiEnable = kTrigEnable,
	kMidiCC = 1 << 1,
};
// One whole pattern, but even that might be limiting?
#define MAX_NOTE_LENGTH 16
struct MidiTrig {
	uint8 flags;
	union {
		uint8 note;
		uint8 cc;
	};
	uint8 ccvalue;
	TQuarterNotes length;

	MidiTrig() : flags(0), ccvalue(0), length(0) {
		note = 0;
	}

	void set_param(uint8 type, double value) {
		switch (type) {
		case kParamMidiTrigEnable:
			set_bit(flags, kTrigEnable, value > 0.5);
			break;
		case kParamMidiTrigNote:
			note = int_param(value, 255);
			break;
		case kParamMidiTrigLength:
			length = int_param(value, MAX_NOTE_LENGTH);
		}
	}

	bool enabled() const {
		return !!(flags & kMidiEnable);
	}
	bool is_note() const {
		return !(flags & kMidiCC);
	}
};

struct DumbBuffer {
	const float *const buffer;
	const size_t length;

	DumbBuffer(const DumbBuffer&) = delete;
	DumbBuffer& operator=(const DumbBuffer&) = delete;
	DumbBuffer(const float *buffer, size_t length) : buffer(buffer), length(length) {}
	~DumbBuffer() { delete[] buffer; }

	static shared_ptr<DumbBuffer> copy_circular(const float *source, size_t position, size_t length) {
		float *output = new float[length];
		auto res = make_shared<DumbBuffer>(output, length);
		size_t n = length;
		while (n--) {
			if (++position == length) position = 0;
			*output++ = source[position];
		}
		return res;
	}

	float safe_get(double position) {
		size_t p = position;
		if (p < 0 || p >= length) {
			return 0;
		}
		return buffer[p];
	}
};

struct Playback {
	shared_ptr<DumbBuffer> source;
	double rate;
	double position;

	Playback() : rate(0), position(0) {}

	bool fill(float *const dst, const int32 channel, const int32 count, double level) const {
		double p = position;
		bool sound = false;
		assert(source);
		for (int32 i = 0; i < count; i++) {
			if (dst[i] = source->safe_get(p) * level) {
				sound = true;
			}
			p += rate;
		}
		return sound;
	}

	void advance(double time) {
		position += time;
		if (position > source->length) {
			position = 0;
			rate = 0;
		}
	}
};

enum SampleTrigFlags {
	kSampleEnable = kTrigEnable,
	kSampleStack = 1 << 1,
	//kSampleOneShot = 1 << 2,
};
struct SampleTrig {
	uint8 flags;
	union {
		// If flags & kSampleStack = 1
		uint8 stack;
		// If flags & kSampleStack = 0
		uint8 sample;
	};
	// compared to original rate in sample, for now a stupid pitching thing, no interpolation
	double rate;

	SampleTrig() : flags(0), stack(0), rate(1.0) {}

	bool enabled() const {
		return !!(flags & kSampleEnable);
	}

	void set_param(uint8 type, double value) {
		switch (type) {
		case kParamSampleTrigEnable:
			set_bit(flags, kSampleEnable, value > 0.5);
			break;
		/*case kParamSampleTrigOneShot:
			set_bit(flags, kSampleOneShot, value > 0.5);
			break;*/
		case kParamSampleTrigStack:
			set_bit(flags, kSampleStack, value > 0.5);
			break;
		case kParamSampleTrigSampleNumber:
			sample = int_param(value, 255);
			break;
		// case kParamSampleTrigRate:
		}
	}
};

struct SampleBuffer {
	float *buffer;
	size_t position, length;

	SampleBuffer() : buffer(NULL), position(0), length(0) {}
	~SampleBuffer() {
		delete[] buffer;
		buffer = NULL;
		position = length = 0;
	}

	void set_length(const size_t new_length) {
		if (new_length == length) return;
		float *new_buffer = new float[new_length];
		size_t new_position = 0;
		assert(new_buffer);
		if (new_length >= length) {
			size_t p = position;
			// copy [p, p) to [0,length), clear out [length, new_length)
			for (size_t n = length; n--;) {
				new_buffer[new_position++] = buffer[p++];
				if (p == length) p = 0;
			}
			memset(new_buffer + new_position, 0, (new_length - length) * sizeof(float));
		} else {
			// copy the most recent samples, [p - new_length, p) to [0, new_length)
			size_t p = position, n = new_length;
			while (n--) {
				new_buffer[n] = buffer[p];
				if (p > 0) {
					p--;
				} else {
					p = length;
				}
			}
		}
		delete[] buffer;
		buffer = new_buffer;
		position = new_position;
		length = new_buffer ? new_length : 0;
	}

	void add_samples(const float *input, size_t n) {
		while (n--) {
			buffer[position++] = *input++;
			if (position == length) position = 0;
		}
	}

	shared_ptr<DumbBuffer> latch() const {
		return DumbBuffer::copy_circular(buffer, position, length);
	}
};

typedef std::deque<shared_ptr<DumbBuffer>> SampleStack;

struct InputChannel
{
	void set_length(const size_t new_length) {
		sampler.set_length(new_length);
	}

	InputChannel() : armed(false) {
		std::fill_n(latch_trigs, PATTERN_LENGTH, false);
		std::fill_n(latch_trig_oneshots, PATTERN_LENGTH, false);
		std::fill_n(direct, NUM_OUTPUTS, 0.0f);
	}
	
	SampleBuffer sampler;
	SampleStack sample_stack;
	float direct[NUM_OUTPUTS];
	// Simple: true to latch the last 4 bars and push them onto the sample stack
	bool latch_trigs[PATTERN_LENGTH];
	bool latch_trig_oneshots[PATTERN_LENGTH];
	bool armed;

	void arm() {
		armed = true;
		// Maybe: set arm flag for each trigger.
	}
	void disarm() {
		armed = false;
	}

	void latch() {
		sample_stack.push_front(sampler.latch());
		if (sample_stack.size() > MAX_STACK_SIZE) {
			sample_stack.pop_back();
		}
	}
};

struct Track {
	// 16 steps (for now)
	MidiTrig midi_trigs[PATTERN_LENGTH];
	SampleTrig sample_trigs[PATTERN_LENGTH];
	Playback playback; // One playback per track, for now
	double level;
	bool armed;

	Track() : level(1.0), armed(false) {}

	void arm() {
		armed = true;
		// Maybe: set arm flag for each trigger.
	}
	void disarm() {
		armed = false;
	}
};

struct VTrackEffect : public AudioEffect {
	// 8 tracks of trigs
	Track tracks[NUM_TRACKS];
	// 4 input channels
	InputChannel input_channels[NUM_INPUTS];

	double tempo;
	// pattern = 4 bars, 16 QNs, used when we don't receive any better position info
	TQuarterNotes positionInPattern;
	float last_vu;

	VTrackEffect() : tempo(0), positionInPattern(0) {
		setControllerClass(VTrackControllerUID);
		set_tempo(120);
		for (int i = 0; i < NUM_INPUTS; i++) {
			for (int o = 0; o < NUM_OUTPUTS; o++) {
				input_channels[i].direct[o] = o < 2 ? 1 : 0;
			}
		}

		input_channels[0].latch_trigs[0] = true;
		input_channels[0].latch_trig_oneshots[0] = true;
		SampleTrig &t = tracks[0].sample_trigs[0];
		t.flags = kSampleStack | kSampleEnable;
		t.stack = 0;
	}

	tresult PLUGIN_API initialize(FUnknown *context) {
		tresult result = AudioEffect::initialize(context);
		// if everything Ok, continue
		if (result != kResultOk) {
			return result;
		}

		addAudioInput(STR16("Stereo In A/B"), SpeakerArr::kStereo);
		addAudioInput(STR16("Stereo In C/D"), SpeakerArr::kStereo);
		addAudioOutput(STR16("Main Out"), SpeakerArr::kStereo);
		addAudioOutput(STR16("Cue Out"), SpeakerArr::kStereo, kAux, 0);

		addEventInput(STR16("Midi In"), 1);
		addEventOutput(STR16("Midi Out"), 1);

		return kResultOk;
	}

	tresult PLUGIN_API setupProcessing(ProcessSetup& newSetup) {
		newSetup.sampleRate = SAMPLE_RATE; // Support only exactly this sample rate. Hope this is a working way to tell the VST host about that :)
		newSetup.symbolicSampleSize = kSample32;
		return AudioEffect::setupProcessing(newSetup);
	}

	static bool hasState(ProcessContext *ctx, uint32 states) {
		return (ctx->state & states) == states;
	}

	void set_tempo(double new_tempo) {
		if (tempo != new_tempo) {
			tempo = new_tempo;
			update_sample_buffers();
		}
	}

	double samples_per_qn() {
		return 60 * SAMPLE_RATE / tempo;
	}

	double samples_per_trig() {
		return samples_per_qn() / TRIGS_PER_QN;
	}

	void update_sample_buffers() {
		// "bar"/"pattern" confusion here - but for now, one bar == one pattern
		size_t samplesPerBar = ceil(samples_per_qn() * PATTERN_LENGTH_QN);
		if (samplesPerBar != input_channels[0].sampler.length) {
			Debug("Tempo changed to %.1f BPM, %lu samples/bar\n", tempo, samplesPerBar);
		}
		for (int i = 0; i < NUM_INPUTS; i++) {
			input_channels[i].set_length(samplesPerBar);
		}
	}

#define BAIL(...) do { Debug(__VA_ARGS__); return kInvalidArgument; } while (0)

	void process_trig_param(const ParamId id, double value) {
		Debug("Track %d trig %d: param %d => %g", id.track, id.trig, id.type, value);
		if (id.track == 0xff || id.trig == 0xff) { // Wildcard changes not handled.
			return;
		}
		bool bv = value > 0.5;
		if (id.type == kParamLatchTrigEnable) {
			input_channels[id.track].latch_trigs[id.trig] = bv;
		} else if (id.type == kParamLatchTrigOneShot) {
			input_channels[id.track].latch_trig_oneshots[id.trig] = bv;
		} else if (id.midi_trig_related()) {
			MidiTrig& trig = tracks[id.track].midi_trigs[id.trig];
			trig.set_param(id.type, value);
		} else {
			SampleTrig& trig = tracks[id.track].sample_trigs[id.trig];
			trig.set_param(id.type, value);
		}
	}

	void process_parameter_queue(IParamValueQueue *vq) {
		int32 offsetSamples;
		int32 numPoints = vq->getPointCount();
		ParamID rawParamID = vq->getParameterId();
		double value;
		if (vq->getPoint(numPoints - 1, offsetSamples, value) != kResultTrue) {
			Debug("Invalid point for param %#x (%d points)\n", rawParamID, numPoints);
			return;
		}
		const ParamId id = ParamId(rawParamID);
		if (id.trig_related()) {
			process_trig_param(id, value);
			return;
		}
		switch (id.type) {
		case kParamArm: {// Perhaps better done as an Event?
			bool arm = value > 0.5;
			assert(id.trig == 0xff); // Only support global trig arming for now
			if (id.track == 0xff) {
				for (int t = 0; t < NUM_TRACKS; t++) {
					arm ? input_channels[t].arm() : input_channels[t].disarm();
				}
			} else if (arm) {
				input_channels[id.track].arm();
			} else {
				input_channels[id.track].disarm();
			}
			break;
		}
		default:
			Debug("Unhandled param %d (%#x value %g)\n", id.type, rawParamID, value);
		}
	}

	void process_trigs(ProcessData& data, TQuarterNotes time, int32 sampleOffset) {
		IEventList *output = data.outputEvents;
		int trig = fmod(time, PATTERN_LENGTH_QN) * TRIGS_PER_QN;
		for (int i = 0; i < NUM_TRACKS; i++) {
			Track& track = tracks[i];
			Event e;
			const MidiTrig& midi = track.midi_trigs[trig];
			if (midi.enabled() && midi.is_note()) {
				Debug("Midi trig: note %d length %.1f", midi.note, midi.length);
				e.type = Event::kNoteOnEvent;
				e.sampleOffset = sampleOffset;
				e.ppqPosition = time;
				e.busIndex = 0;
				e.noteOn.channel = i;
				e.noteOn.pitch = midi.note;
				e.noteOn.length = midi.length * samples_per_qn();
				e.noteOn.noteId = -1;
				output->addEvent(e);
				e.type = Event::kNoteOffEvent;
				e.sampleOffset += e.noteOn.length;
				e.ppqPosition += midi.length;
				e.noteOff.channel = i;
				e.noteOff.pitch = midi.note;
				e.noteOff.noteId = -1;
				output->addEvent(e);
			}
			const SampleTrig& sample = track.sample_trigs[trig];
			if (sample.enabled()) {
				Playback& playback = track.playback;
				if (sample.flags & kSampleStack) {
					uint8 input = sample.stack / MAX_STACK_SIZE;
					uint8 stack = sample.stack % MAX_STACK_SIZE;
					Debug("Sample trig %d @%.1f: input %d/stack %d rate %.1fs\n", trig, time, input, stack, sample.rate);
					// TODO Move the sample stack from InputChannel to Track, let Latch trigs include source channel.
					if (input >= NUM_INPUTS) {
						Debug("Invalid input %d >= %d\n", input, NUM_INPUTS);
						continue;
					}
					const SampleStack& sample_stack = input_channels[input].sample_stack;
					if (stack >= sample_stack.size()) {
						Debug("Invalid stack index %d >= %d\n", stack, sample_stack.size());
						continue;
					}
					playback.source = sample_stack[stack];
				}
				playback.rate = sample.rate;
				playback.position = 0;
			}
		}
		for (int i = 0; i < NUM_INPUTS; i++) {
			InputChannel& chan = input_channels[i];
			if (!chan.latch_trigs[trig]) continue;
			if (chan.latch_trig_oneshots[trig]) {
				if (!chan.armed) continue;
				chan.armed = false;
			}
			Debug("Latch trig %d @%.1f: input channel %d, oneshot=%d\n", trig, time, i, chan.latch_trig_oneshots[trig]);
			chan.latch();
			//Debug("Channel %d: now %u stacked\n", i, (unsigned)chan.sample_stack.size());
		}
	}

	tresult PLUGIN_API process(ProcessData& data) {
		if (IParameterChanges *params = data.inputParameterChanges) {
			const int32 n = params->getParameterCount();
			for (int32 i = n; i < n; i++) {
				process_parameter_queue(params->getParameterData(i));
			}
		}
		if (data.processContext) {
			update_context(data.processContext);
		}
		copy_events(data);
		reset_silence(data.outputs, data.numOutputs);
		int32 sample_position = 0;
		TQuarterNotes musicTime = positionInPattern;
		while (sample_position < data.numSamples) {
			int32 next_event = process_events(data, sample_position);
			int32 next_trig = get_next_trig(musicTime, sample_position);
			if (next_trig == sample_position) {
				process_trigs(data, musicTime, sample_position);
				next_trig += samples_per_trig();
			}
			int32 next_sample_pos = min(data.numSamples, next_trig);
			if (next_event >= 0) {
				next_sample_pos = min(next_sample_pos, next_event);
			}
			int32 num_samples = next_sample_pos - sample_position;
			assert(next_sample_pos <= data.numSamples);
			if (num_samples) {
				process(data, sample_position, num_samples);
			}

			musicTime += num_samples / samples_per_qn();
			if (musicTime > PATTERN_LENGTH_QN) musicTime -= PATTERN_LENGTH_QN;
			sample_position = next_sample_pos;
		}
		positionInPattern = musicTime;

		float in_vu = get_vu(data.inputs, data.numInputs, data.numSamples);
		float out_vu = get_vu(data.outputs, data.numOutputs, data.numSamples);
		float vu = out_vu;
		IParameterChanges* paramChanges = data.outputParameterChanges;
		if (paramChanges && last_vu != vu) {
			int32 index = 0;
			IParamValueQueue* paramQueue = paramChanges->addParameterData(kVuPPMId, index);
			if (paramQueue) {
				int32 index2 = 0;
				paramQueue->addPoint(0, vu, index2);
			}
			last_vu = vu;
		}

		return kResultOk;
	}

	int32 get_next_trig(TQuarterNotes qn, int32 samples) {
		TQuarterNotes next_trig_qn = ceil(qn * TRIGS_PER_QN) / TRIGS_PER_QN;
		return samples + (next_trig_qn - qn) * samples_per_qn();
	}

	int32 get_next_qn(TQuarterNotes qn, int32 samples) {
		TQuarterNotes next_qn = ceil(qn);
		return samples + (next_qn - qn) * samples_per_qn();
	}

	void set_position(TQuarterNotes barPosition, TQuarterNotes projectTime) {
		// Last bar was at (project time) barPosition. We don't really care here but just use the project time directly.
		positionInPattern = fmod(projectTime, PATTERN_LENGTH_QN);
	}

	void update_context(ProcessContext* ctx) {
		if (hasState(ctx, ProcessContext::kBarPositionValid | ProcessContext::kProjectTimeMusicValid)) {
			set_position(ctx->barPositionMusic, ctx->projectTimeMusic);
		}
		if (hasState(ctx, ProcessContext::kTempoValid)) {
			set_tempo(ctx->tempo);
		}
	}

	void copy_events(ProcessData& data) {
		if (IEventList *events = data.inputEvents) {
			int32 n = events->getEventCount(), i = 0;
			if (!n) return;
			IEventList *out = data.outputEvents;
			Event e;
			while (n--) {
				// e.sampleOffset and e.ppqPosition
				events->getEvent(i++, e);
				if (e.type != Event::kDataEvent) {
					Debug("Event @%.1f (%d) on bus %d: type %d\n", e.ppqPosition, e.sampleOffset, e.busIndex, e.type);
				}
				out->addEvent(e);
			}
		}
	}

	// Returns the sample position of the next event to process
	int32 process_events(ProcessData& data, const int32 sample_position) {
		if (IEventList *events = data.inputEvents) {
			const int32 n = events->getEventCount();
			Event e;
			for (int32 i = 0; i < n; i++) {
				events->getEvent(i, e);
				if (e.sampleOffset < sample_position) {
					continue;
				} else if (e.sampleOffset > sample_position) {
					return e.sampleOffset;
				}
				// TODO Actually process events :)
			}
		}
		return -1;
	}

	void reset_silence(AudioBusBuffers* outputs, int32 numOutputs) {
		for (int32 bus = 0; bus < numOutputs; bus++) {
			reset_silence(outputs[bus]);
		}
	}

	void reset_silence(AudioBusBuffers& output) {
		output.silenceFlags = (1 << output.numChannels) - 1;
	}

	float get_vu(AudioBusBuffers *buses, size_t numBuses, size_t numSamples) {
		float vu = 0;
		for (int32 bus = 0; bus < numBuses; bus++) {
			AudioBusBuffers &outp = buses[bus];
			for (int32 c = 0; c < outp.numChannels; c++) {
				float *src = outp.channelBuffers32[c];
				for (int32 s = 0; s < numSamples; s++) {
					vu = max(vu, src[s]);
				}
			}
		}
		return vu;
	}

	void process(ProcessData& data, const int32 offset, const int32 count) {
		process_inputs(data, offset, count);
		process_samples(data, offset, count);
	}

	void process_samples(ProcessData& data, const int32 offset, const int32 count) {
		AudioBusBuffers &outp = data.outputs[0];
		for (int i = 0; i < NUM_TRACKS; i++) {
			Playback& playback = tracks[i].playback;
			if (!playback.rate) continue;

			for (int32 c = 0; c < 2; c++) {
				float *dst = outp.channelBuffers32[c] + offset;
				if (playback.fill(dst, c, count, tracks[i].level)) {
					outp.silenceFlags &= ~(1ull << c);
				}
			}
			playback.advance(count);
		}
	}

	void process_inputs(ProcessData& data, const int32 offset, const int32 count) {
		assert(data.numOutputs >= 1);
		assert(count > 0);
		assert(offset < data.numSamples);
		assert(offset + count > offset && offset + count <= data.numSamples);

		AudioBusBuffers &outp = data.outputs[0];
		size_t input_channel_index = 0;
		for (int32 bus = 0; bus < data.numInputs; bus++) {
			AudioBusBuffers &inp = data.inputs[bus];
			assert(outp.numChannels == 2);
			for (int32 c = 0; c < inp.numChannels; c++) {
				InputChannel& chan = input_channels[input_channel_index];
				float *src = inp.channelBuffers32[c] + offset;
				chan.sampler.add_samples(src, count);
				for (int32 outc = 0; outc < 2; outc++) {
					float *dst = outp.channelBuffers32[outc] + offset;
					bool silent = !!(inp.silenceFlags & (1ull << c));
					if (silent) {
						memset(dst, 0, count * sizeof(float));
					} else {
						float f = chan.direct[outc];
						silent = true;
						for (int32 s = count; s--;) {
							if (*dst++ = *src++ * f) {
								silent = false;
							}
						}
					}
					// output channels are set silent by default, then cleared whenever we output a non-zero sample on them.
					if (!silent) {
						outp.silenceFlags &= ~(1ull << outc);
					}
				}
				input_channel_index++;
			}
		}
	}

tresult PLUGIN_API terminate()
{
	HERE;
	return AudioEffect::terminate();
}


~VTrackEffect()
{
	HERE;
}

};

FUnknown *createVTrackEffect(void *context) {
	return (IAudioProcessor*)new VTrackEffect();
}
