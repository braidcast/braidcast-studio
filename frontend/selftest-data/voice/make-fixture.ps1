# Generates the voice recognizer's test fixtures with the Windows speech synthesizer,
# so they are small synthesized WAVs rather than a recording of anyone's voice.
#
# The repository does not carry them yet: the owner runs this once on Windows and
# commits the two WAVs it writes to frontend/selftest-data/voice/fixtures/. Until then
# the self-test cases that need them (voice-wav, voice-recognizer, voice-vad and
# voice-continuous) report SKIPPED, naming the missing file.
#
# Run from the repository root:
#   powershell.exe -ExecutionPolicy Bypass -File frontend/selftest-data/voice/make-fixture.ps1
# Once committed, regenerate only if a fixture is lost: a different voice changes what
# the model hears, and the recognizer self-test asserts on the text.
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Speech

$outDir = Join-Path $PSScriptRoot "fixtures"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$outFile = Join-Path $outDir "switch-to-gameplay.wav"

$synth = New-Object System.Speech.Synthesis.SpeechSynthesizer
# 16 kHz mono 16-bit is what whisper wants, so nothing has to resample the fixture.
$format = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(16000, [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen, [System.Speech.AudioFormat.AudioChannel]::Mono)
$synth.SetOutputToWaveFile($outFile, $format)
$synth.Rate = 0
$synth.Speak("Switch to gameplay.")
$synth.SetOutputToNull()

# The always-listen fixture: the same command, opened by the default wake phrase.
$wokenFile = Join-Path $outDir "braidcast-switch-to-gameplay.wav"
$synth.SetOutputToWaveFile($wokenFile, $format)
$synth.Speak("Braidcast, switch to gameplay.")
$synth.SetOutputToNull()
Write-Output "wrote $wokenFile ($((Get-Item $wokenFile).Length) bytes)"
$synth.Dispose()

$size = (Get-Item $outFile).Length
Write-Output "wrote $outFile ($size bytes)"
