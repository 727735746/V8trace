(async function mediaFixture() {
  const must = (value, reason) => { if (!value) throw new Error(reason); };
  const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
  const report = { playback: [], roundtrips: [] };
  function event(target, name, timeout = 15000) {
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error(name + ' timeout')), timeout);
      target.addEventListener(name, () => { clearTimeout(timer); resolve(); }, {once: true});
      target.addEventListener('error', () => { clearTimeout(timer); reject(new Error('media error ' + (target.error && target.error.code))); }, {once: true});
    });
  }
  async function play(src, minimumTime = 1.7, minimumFrames = 10) {
    const video = document.createElement('video');
    video.muted = true;
    video.playsInline = true;
    video.width = 160;
    video.height = 96;
    document.body.appendChild(video);
    let callbacks = 0;
    const frame = () => { callbacks++; if (!video.ended) video.requestVideoFrameCallback(frame); };
    video.requestVideoFrameCallback(frame);
    video.src = src;
    const ended = event(video, 'ended');
    await video.play();
    await ended;
    const quality = video.getVideoPlaybackQuality();
    must(video.currentTime >= minimumTime && quality.totalVideoFrames >= minimumFrames && callbacks > 0,
      'No actual video progress/decoded frames: ' + src);
    must(video.videoWidth === 160 && video.videoHeight === 96, 'Unexpected decoded dimensions');
    const result = {src, time: video.currentTime, totalFrames: quality.totalVideoFrames,
      droppedFrames: quality.droppedVideoFrames, callbacks, width: video.videoWidth, height: video.videoHeight};
    video.remove();
    return result;
  }
  async function roundtrip(codec) {
    const canvas = document.createElement('canvas');
    canvas.width = 160; canvas.height = 96;
    const context = canvas.getContext('2d');
    const config = {codec, width: 160, height: 96, bitrate: 180000, framerate: 15, hardwareAcceleration: 'prefer-software'};
    if (codec.startsWith('avc1')) config.avc = {format: 'annexb'};
    const supported = await VideoEncoder.isConfigSupported(config);
    must(supported.supported, 'Software encoder unsupported: ' + codec);
    const chunks = [], errors = [];
    let decoderConfig;
    const encoder = new VideoEncoder({output(chunk, meta) { chunks.push(chunk); decoderConfig = decoderConfig || meta.decoderConfig; },
      error(error) { errors.push(String(error)); }});
    encoder.configure(config);
    for (let index = 0; index < 12; index++) {
      context.fillStyle = 'rgb(' + (index * 19) + ',80,150)';
      context.fillRect(0, 0, 160, 96);
      const frame = new VideoFrame(canvas, {timestamp: index * 66667, duration: 66667});
      encoder.encode(frame, {keyFrame: index === 0});
      frame.close();
    }
    await encoder.flush(); encoder.close();
    must(chunks.length > 0 && !errors.length, 'No actual encoded output: ' + codec);
    let decoded = 0;
    const decoder = new VideoDecoder({output(frame) { decoded++; frame.close(); }, error(error) { errors.push(String(error)); }});
    decoder.configure(decoderConfig || {codec, codedWidth: 160, codedHeight: 96});
    for (const chunk of chunks) decoder.decode(chunk);
    await decoder.flush(); decoder.close();
    must(decoded === 12 && !errors.length, 'Encoded chunks did not roundtrip: ' + codec + ' ' + JSON.stringify(errors));
    return {codec, chunks: chunks.length, bytes: chunks.reduce((total, chunk) => total + chunk.byteLength, 0), decoded, acceleration: 'prefer-software'};
  }
  try {
    report.playback.push(await play('/fixture.mp4'));
    report.playback.push(await play('/fixture.webm'));
    const audio = new OfflineAudioContext(1, 1, 48000);
    const decodedAudio = await audio.decodeAudioData(await (await fetch('/fixture.mp4')).arrayBuffer());
    must(decodedAudio.length >= 72000 && decodedAudio.numberOfChannels > 0, 'AAC audio was not decoded');
    report.aac = {frames: decodedAudio.length, sampleRate: decodedAudio.sampleRate, channels: decodedAudio.numberOfChannels};
    for (const codec of ['avc1.42001E', 'vp8', 'vp09.00.10.08']) report.roundtrips.push(await roundtrip(codec));
    const canvas = document.createElement('canvas'); canvas.width = 160; canvas.height = 96;
    const context = canvas.getContext('2d'), stream = canvas.captureStream(15), parts = [];
    const recorder = new MediaRecorder(stream, {mimeType: 'video/webm;codecs=vp8'});
    recorder.ondataavailable = event => { if (event.data.size) parts.push(event.data); };
    const stopped = event(recorder, 'stop');
    recorder.start(100);
    for (let index = 0; index < 16; index++) {
      context.fillStyle = index % 2 ? 'blue' : 'orange'; context.fillRect(0, 0, 160, 96); await sleep(80);
    }
    recorder.stop(); await stopped; stream.getTracks().forEach(track => track.stop());
    const blob = new Blob(parts, {type: recorder.mimeType});
    must(blob.size > 200, 'MediaRecorder produced no video bytes');
    const url = URL.createObjectURL(blob);
    report.recording = {bytes: blob.size, mimeType: blob.type, playback: await play(url, 0.5, 5)};
    URL.revokeObjectURL(url);
    report.ok = true;
  } catch (error) {
    report.ok = false; report.error = String(error); report.stack = error.stack;
  }
  await fetch('/done', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(report)});
})();
