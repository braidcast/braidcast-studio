# Generates the voice recognizer's test fixtures with the Windows speech synthesizer,
# so the repository carries a small WAV rather than a recording of anyone's voice.
# Run from the repository root:
#   powershell.exe -ExecutionPolicy Bypass -File frontend/selftest-data/voice/make-fixture.ps1
# Regenerate only if a fixture is lost: a different voice changes what the model
# hears, and the recognizer self-test asserts on the text.
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
