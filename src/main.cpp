#include <Arduino.h>
#include <Bounce2.h>
#include <Keypad.h>
#include <Audio.h>
#include <SD.h>
#include <SPI.h>
#include <recordingstate.h>

const int BUTTON_PIN = 0;
const int SWITCH_PIN = 4;

// TODO: verify this against your Rev D shield's silkscreen/datasheet -
// 10 is the SD chip-select pin on PJRC's official audio shield, but
// third-party revisions have been known to differ.
#define SDCARD_CS_PIN 10

// WAV format written for every recording
const uint32_t SAMPLE_RATE = 44100;
const uint16_t BITS_PER_SAMPLE = 16;
const uint16_t NUM_CHANNELS = 1;

// how many audio blocks to accumulate before each SD write
const int NBLOX = 4;

const char* COUNTER_FILE = "COUNTER.TXT";

// how long to wait after the last digit before treating what's typed as submitted
const unsigned long DIGIT_TIMEOUT_MS = 1500;

bool MODE; //0 is playback mode, 1 is recording mode
RecordingState state = RecordingState::Idle;

Bounce phoneButton = Bounce();
Bounce hookSwitch = Bounce();

const byte ROWS = 4;
const byte COLS = 3;

char keys[ROWS][COLS] = {
  {'1', '2', '3'},
  {'4', '5', '6'},
  {'7', '8', '9'},
  {'*', '0', '#'}
};

byte rowPins[ROWS] = {14, 15, 17, 16};  // R1, R2, R3, R4
byte colPins[COLS] = {5, 6, 9};         // C1, C2, C3

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// Playback path: SD card -> promptPlayer -> I2S output (the shield's codec).
// Shared by the recording-mode prompt/beep and playback-mode announcements/
// recordings - safe because MODE gates which flow is active, so they never
// run at the same time.
AudioPlaySdWav promptPlayer;
AudioOutputI2S audioOutput;
AudioConnection patchCordL(promptPlayer, 0, audioOutput, 0);
AudioConnection patchCordR(promptPlayer, 1, audioOutput, 1);

// Recording path: I2S input (mic) -> recordQueue -> we drain it to SD ourselves
AudioInputI2S audioInput;
AudioRecordQueue recordQueue;
AudioConnection patchCordRecordIn(audioInput, 0, recordQueue, 0);

AudioControlSGTL5000 sgtl5000;

File frec;
uint32_t recByteSaved = 0;
uint32_t nextRecordingId = 0;
char recordingFilename[13]; // "RECxxxxx.WAV" (8.3 filename) + null terminator

// --- playback mode state ---
bool playbackActive = false;   // true while the phone is off-hook in playback mode
int currentFileIndex = -1;     // 0-based id of the last file played; -1 = none yet

const int MAX_QUEUE = 10;
char audioQueue[MAX_QUEUE][13];
int audioQueueLength = 0;
int audioQueueIndex = 0;
bool audioQueueActive = false;

char digitBuffer[6]; // up to 5 digits + null
uint8_t digitCount = 0;
unsigned long lastDigitPressTime = 0;

void OnPhoneButtonPickedUp();
void OnPhoneButtonPutDown();
void PlayPrompt();
void WriteWavHeader(File &f, uint32_t dataBytes);
uint32_t LoadNextRecordingId();
void SaveNextRecordingId(uint32_t id);
void StartRecording();
void ContinueRecording();
void StopRecording();
void ClearAudioQueue();
void EnqueueAudio(const char* filename);
void StartAudioQueue();
void UpdateAudioQueue();
void EnqueueNumberDigits(uint32_t number);
void AnnounceAvailableCount();
void PlaySelectedRecording(uint32_t selection);
void PlayAdjacentRecording(int direction);
void HandlePlaybackKeypad();
void CheckDigitTimeout();

void setup() {
  Serial.begin(9600);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  phoneButton.attach(BUTTON_PIN);
  phoneButton.interval(10);

  pinMode(SWITCH_PIN, INPUT_PULLUP);
  hookSwitch.attach(SWITCH_PIN);
  hookSwitch.interval(10);
  MODE = hookSwitch.read();

  AudioMemory(60);
  sgtl5000.enable();
  sgtl5000.volume(0.5);

  if (!SD.begin(SDCARD_CS_PIN)) {
    Serial.println("SD card init failed - check SDCARD_CS_PIN");
  }

  nextRecordingId = LoadNextRecordingId();
}

void loop() {
  //phone picked up or not
  phoneButton.update();
  if (phoneButton.changed() && phoneButton.read() == LOW) { //phone was picked up
    OnPhoneButtonPickedUp();
  }
  else if(phoneButton.changed() && phoneButton.read() == HIGH){ //phone was set Down
    OnPhoneButtonPutDown();
  }

  //check if record switch changed
  hookSwitch.update();
  if (hookSwitch.changed() && hookSwitch.read() == LOW) { //Playback Mode
    MODE = false;
  }
  else if(hookSwitch.changed() && hookSwitch.read() == HIGH) { //Recording Mode
    MODE = true;
  }

  // advance the recording-mode state machine once the current audio finishes
  if (state == RecordingState::PlayingPrompt && !promptPlayer.isPlaying()) {
    //prompt finished -> next is the beep (not implemented yet)
    state = RecordingState::PlayingBeep;
  }

  // recording must be drained every loop pass or the buffer overflows
  if (state == RecordingState::Recording) {
    ContinueRecording();
  }

  // playback mode: keep the queued announcement/recording moving forward,
  // and keep listening for a keypress the whole time so it can interrupt
  if (playbackActive) {
    UpdateAudioQueue();
    HandlePlaybackKeypad();
    CheckDigitTimeout();
  }
}

void OnPhoneButtonPickedUp() {
  if(MODE == true) //recording mode
  {
    if(state == RecordingState::Idle) {
      PlayPrompt();
    }
  }
  else //playback mode
  {
    playbackActive = true;
    currentFileIndex = -1;
    digitCount = 0;
    AnnounceAvailableCount();
  }
}

void OnPhoneButtonPutDown() {
  if(MODE == true) //recording mode
  {
    // hung up before the prompt finished on its own - cut it off immediately
    if (state == RecordingState::PlayingPrompt) {
      promptPlayer.stop();
    }

    if(state == RecordingState::Recording) {
      state = RecordingState::Saving;

      StopRecording();

      //switch to idle
      state = RecordingState::Idle;
    } else {
      state = RecordingState::Idle;
    }
  }
  else //playback mode
  {
    promptPlayer.stop();
    ClearAudioQueue();
    digitCount = 0;
    playbackActive = false;
  }
}

void PlayPrompt() {
  state = RecordingState::PlayingPrompt;
  // file must exist at the root of the SD card, 8.3 filename, PCM WAV
  promptPlayer.play("PROMPT.WAV");

  while (promptPlayer.isPlaying()) {
    // busy-wait until the file finishes
  }

  state = RecordingState::PlayingBeep;
  promptPlayer.play("BEEP.WAV");
  while (promptPlayer.isPlaying()){
  }

  state = RecordingState::Recording;
  StartRecording();
}

// Writes a canonical 44-byte PCM WAV header at the file's current position.
// Called twice per recording: once as a placeholder before any audio is
// written, and again (after seeking back to 0) once the real byte count
// is known, since the header's size fields can't be known until the
// recording ends.
void WriteWavHeader(File &f, uint32_t dataBytes) {
  uint32_t byteRate = SAMPLE_RATE * NUM_CHANNELS * BITS_PER_SAMPLE / 8;
  uint16_t blockAlign = NUM_CHANNELS * BITS_PER_SAMPLE / 8;
  uint32_t chunkSize = 36 + dataBytes;
  uint32_t subchunk1Size = 16;
  uint16_t audioFormat = 1; // PCM

  f.write("RIFF");
  f.write((byte*)&chunkSize, 4);
  f.write("WAVE");
  f.write("fmt ");
  f.write((byte*)&subchunk1Size, 4);
  f.write((byte*)&audioFormat, 2);
  f.write((byte*)&NUM_CHANNELS, 2);
  f.write((byte*)&SAMPLE_RATE, 4);
  f.write((byte*)&byteRate, 4);
  f.write((byte*)&blockAlign, 2);
  f.write((byte*)&BITS_PER_SAMPLE, 2);
  f.write("data");
  f.write((byte*)&dataBytes, 4);
}

// Reads the next recording ID from COUNTER_FILE on SD. Returns 0 if the
// file doesn't exist yet (first recording ever made on this card).
uint32_t LoadNextRecordingId() {
  File f = SD.open(COUNTER_FILE, FILE_READ);
  if (!f) {
    return 0;
  }
  uint32_t id = f.parseInt();
  f.close();
  return id;
}

void SaveNextRecordingId(uint32_t id) {
  SD.remove(COUNTER_FILE);
  File f = SD.open(COUNTER_FILE, FILE_WRITE);
  if (f) {
    f.println(id);
    f.close();
  }
}

void StartRecording() {
  snprintf(recordingFilename, sizeof(recordingFilename), "REC%05lu.WAV", nextRecordingId);

  if (SD.exists(recordingFilename)) {
    SD.remove(recordingFilename);
  }

  frec = SD.open(recordingFilename, FILE_WRITE);
  if (!frec) {
    Serial.println("Failed to open recording file");
    return;
  }

  recByteSaved = 0;
  WriteWavHeader(frec, 0); // placeholder - patched with the real size in StopRecording()

  recordQueue.begin();
}

void ContinueRecording() {
  if (recordQueue.available() >= NBLOX) {
    byte buffer[NBLOX * AUDIO_BLOCK_SAMPLES * sizeof(int16_t)];
    for (int i = 0; i < NBLOX; i++) {
      memcpy(buffer + i * AUDIO_BLOCK_SAMPLES * sizeof(int16_t),
             recordQueue.readBuffer(), AUDIO_BLOCK_SAMPLES * sizeof(int16_t));
      recordQueue.freeBuffer();
    }
    frec.write(buffer, sizeof(buffer));
    recByteSaved += sizeof(buffer);
  }
}

void StopRecording() {
  recordQueue.end();
  while (recordQueue.available() > 0) {
    frec.write((byte*)recordQueue.readBuffer(), AUDIO_BLOCK_SAMPLES * sizeof(int16_t));
    recordQueue.freeBuffer();
    recByteSaved += AUDIO_BLOCK_SAMPLES * sizeof(int16_t);
  }

  frec.seek(0);
  WriteWavHeader(frec, recByteSaved);
  frec.close();

  nextRecordingId++;
  SaveNextRecordingId(nextRecordingId);
}

// --- playback mode ---

// A small non-blocking playlist: EnqueueAudio() while building up what to
// say, then StartAudioQueue() once, then UpdateAudioQueue() every loop pass
// advances to the next file automatically once the current one finishes.
// This (rather than PlayPrompt's blocking style) is what lets a keypress
// interrupt playback - loop() never stops polling the keypad while it plays.

void ClearAudioQueue() {
  audioQueueLength = 0;
  audioQueueIndex = 0;
  audioQueueActive = false;
}

void EnqueueAudio(const char* filename) {
  if (audioQueueLength < MAX_QUEUE) {
    snprintf(audioQueue[audioQueueLength], sizeof(audioQueue[0]), "%s", filename);
    audioQueueLength++;
  }
}

void StartAudioQueue() {
  audioQueueIndex = 0;
  if (audioQueueLength > 0) {
    audioQueueActive = true;
    promptPlayer.play(audioQueue[0]);
  } else {
    audioQueueActive = false;
  }
}

void UpdateAudioQueue() {
  if (!audioQueueActive || promptPlayer.isPlaying()) {
    return;
  }

  audioQueueIndex++;
  if (audioQueueIndex < audioQueueLength) {
    promptPlayer.play(audioQueue[audioQueueIndex]);
  } else {
    audioQueueActive = false;
  }
}

// Spells a number out digit by digit using 0.WAV..9.WAV (no text-to-speech
// available, so "12" is announced as "one" "two", not "twelve").
void EnqueueNumberDigits(uint32_t number) {
  char digits[11];
  snprintf(digits, sizeof(digits), "%lu", (unsigned long)number);
  for (int i = 0; digits[i] != '\0'; i++) {
    char filename[8];
    snprintf(filename, sizeof(filename), "%c.WAV", digits[i]);
    EnqueueAudio(filename);
  }
}

// "There are" + <digits of nextRecordingId> + "recordings available"
void AnnounceAvailableCount() {
  ClearAudioQueue();
  EnqueueAudio("COUNT.WAV");
  EnqueueNumberDigits(nextRecordingId);
  EnqueueAudio("AVAIL.WAV");
  StartAudioQueue();
}

// selection is 1-indexed as typed by the caller (recording #1 == REC00000.WAV)
void PlaySelectedRecording(uint32_t selection) {
  ClearAudioQueue();

  if (selection < 1 || selection > nextRecordingId) {
    EnqueueNumberDigits(selection);
    EnqueueAudio("NOTAVAIL.WAV");
  } else {
    currentFileIndex = selection - 1;
    EnqueueAudio("PLAYING.WAV");
    EnqueueNumberDigits(selection);

    char filename[13];
    snprintf(filename, sizeof(filename), "REC%05lu.WAV", (unsigned long)currentFileIndex);
    EnqueueAudio(filename);
  }

  StartAudioQueue();
}

// direction: -1 for '*' (previous), +1 for '#' (next). Wraps around, and
// plays the file directly with no "Playing audio file X" announcement.
void PlayAdjacentRecording(int direction) {
  if (nextRecordingId == 0) {
    return; // nothing recorded yet
  }

  if (currentFileIndex < 0) {
    currentFileIndex = (direction > 0) ? 0 : (int)(nextRecordingId - 1);
  } else {
    currentFileIndex = ((currentFileIndex + direction) % (int)nextRecordingId + (int)nextRecordingId) % (int)nextRecordingId;
  }

  ClearAudioQueue();
  char filename[13];
  snprintf(filename, sizeof(filename), "REC%05lu.WAV", (unsigned long)currentFileIndex);
  EnqueueAudio(filename);
  StartAudioQueue();
}

void HandlePlaybackKeypad() {
  char key = keypad.getKey();
  if (!key) {
    return;
  }

  // any keypress cuts off whatever's currently playing
  if (audioQueueActive) {
    promptPlayer.stop();
    ClearAudioQueue();
  }

  if (key == '*' || key == '#') {
    digitCount = 0; // discard any number that was being typed
    PlayAdjacentRecording(key == '#' ? 1 : -1);
    return;
  }

  if (key >= '0' && key <= '9' && digitCount < sizeof(digitBuffer) - 1) {
    digitBuffer[digitCount++] = key;
    digitBuffer[digitCount] = '\0';
    lastDigitPressTime = millis();
  }
}

void CheckDigitTimeout() {
  if (digitCount > 0 && millis() - lastDigitPressTime >= DIGIT_TIMEOUT_MS) {
    uint32_t selection = atoi(digitBuffer);
    digitCount = 0;
    PlaySelectedRecording(selection);
  }
}
