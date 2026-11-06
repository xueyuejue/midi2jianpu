#!/usr/bin/env node
/**
 * WASM 冒烟测试：校验 docs/wasm/ 里提交的产物是否真的具备网页所需的全部能力。
 *
 * 背景：这个仓库曾经出现过「源码已加 MusicXML 导出，但 docs/wasm/ 里的
 * midi2jianpu.js 是旧的，缺 _midi2jianpu_musicxml」的静默回归 —— CLI 全绿、
 * 网页按钮点了没反应。CLI 测试永远发现不了这类问题，只能由本脚本兜住。
 *
 * 用法：
 *     node tests/wasm_smoke.mjs [midi2jianpu.js 路径]
 * 默认路径：docs/wasm/midi2jianpu.js
 *
 * 退出码非 0 即失败，可直接用于 CI。
 */
import { createRequire } from 'node:module';
import { readFileSync, existsSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(HERE, '..');
const FIXTURES = path.join(ROOT, 'tests', 'fixtures');

const jsPath = path.resolve(process.argv[2] || path.join(ROOT, 'docs', 'wasm', 'midi2jianpu.js'));

const failures = [];
function check(cond, msg, extra) {
  if (cond) {
    console.log(`  [PASS] ${msg}`);
  } else {
    console.log(`  [FAIL] ${msg}`);
    if (extra) console.log(`         ${String(extra).slice(0, 300)}`);
    failures.push(msg);
  }
}

/** 网页 docs/index.html 里 callWasm() 的同款调用路径，顺便验证它没写错 */
function makeCaller(mod) {
  let midi = null;
  let lyrics = '';

  function setMidi(bytes) {
    midi = bytes;
  }
  function setLyrics(txt) {
    lyrics = txt || '';
  }
  function call(fnName, keyPitch, isMajor) {
    const fn = mod['_' + fnName];
    if (typeof fn !== 'function') return null;

    const ptr = mod._malloc(midi.length);
    mod.HEAPU8.set(midi, ptr);

    if (mod._midi2jianpu_set_lyrics) {
      const n = mod.lengthBytesUTF8(lyrics) + 1;
      const lp = mod._malloc(n);
      mod.stringToUTF8(lyrics, lp, n);
      mod._midi2jianpu_set_lyrics(lp);
      mod._free(lp);
    }

    fn(ptr, midi.length, keyPitch, isMajor);
    const len = mod._midi2jianpu_result_len();
    const buf = mod._malloc(len + 1);
    mod._midi2jianpu_copy_result(buf, len + 1);
    const out = mod.UTF8ToString(buf);

    mod._free(ptr);
    mod._free(buf);
    return out;
  }
  return { setMidi, setLyrics, call };
}

const sampleMidi = readFileSync(path.join(FIXTURES, 'sample.mid'));
const humanMidi = readFileSync(path.join(FIXTURES, 'humanized.mid'));
const sampleLrc = readFileSync(path.join(FIXTURES, 'sample.lrc'), 'utf8');

check(existsSync(jsPath), `产物存在: ${path.relative(ROOT, jsPath)}`);

const require = createRequire(import.meta.url);
let factory = null;
try {
  factory = require(jsPath);
} catch (e) {
  console.log(`  [FAIL] JS 胶水层无法加载: ${e.message}\n`);
  console.log('先编译：emcmake cmake -B build-wasm -S . -DM2J_BUILD_WASM=ON && cmake --build build-wasm');
  process.exit(1);
}

check(typeof factory === 'function', 'JS 胶水层可加载（MODULARIZE 导出为函数）');

const mod = await factory({
  locateFile: (f) => path.join(path.dirname(jsPath), f),
});
const c = makeCaller(mod);

// ---- 1. 导出符号齐全：网页能点到的每个按钮背后都得有函数 ----
const REQUIRED = [
  '_midi2jianpu_parse',
  '_midi2jianpu_html',
  '_midi2jianpu_musicxml', // 曾经缺失的那个
  '_midi2jianpu_set_lyrics',
  '_midi2jianpu_get_notes_json',
  '_midi2jianpu_result_len',
  '_midi2jianpu_copy_result',
  '_malloc',
  '_free',
];
for (const s of REQUIRED) {
  check(typeof mod[s] === 'function', `导出符号 ${s}`);
}
// 新版 Emscripten 不再默认暴露内存视图；网页靠它把 MIDI 字节写进 wasm
check(mod.HEAPU8 instanceof Uint8Array, '导出 HEAPU8 内存视图');

// ---- 2. 文本简谱 ----
c.setMidi(sampleMidi);
c.setLyrics('');
const text = c.call('midi2jianpu_parse', -1, 1);
check(text && text.length > 0, '文本简谱非空');
check(/1=C/.test(text || ''), '文本简谱含调号（首调写法 1=C）', text);
check(!/错误/.test(text || ''), '文本简谱无错误信息', text);

// ---- 3. HTML 谱面 ----
const html = c.call('midi2jianpu_html', -1, 1);
check(html && html.includes('class=\'staff\''), 'HTML 含谱面容器');
check(html && html.includes('class=\'measure\''), 'HTML 含小节');
check(html && html.includes('class=\'row\''), 'HTML 含行分组（每行固定小节数）');

// ---- 4. MusicXML（本次重点）----
const xml = c.call('midi2jianpu_musicxml', -1, 1);
check(xml && xml.includes('<score-partwise'), 'MusicXML 含 score-partwise 根元素',
  xml && xml.slice(0, 200));
check(xml && xml.includes('<attributes>'), 'MusicXML 含 attributes（MuseScore 能打开的前提）', xml);
check(xml && xml.includes('<divisions>'), 'MusicXML 含 divisions');
check(xml && xml.includes('<key>'), 'MusicXML 含调号');
check(xml && xml.includes('<time>'), 'MusicXML 含拍号');
check(xml && xml.includes('<note>'), 'MusicXML 含音符');
check(xml && !xml.includes('<error>'), 'MusicXML 无错误节点');

// ---- 5. 歌词：设置后应一字一音地出现 ----
c.setMidi(sampleMidi);
c.setLyrics(sampleLrc);
const htmlLyric = c.call('midi2jianpu_html', -1, 1);
const firstChar = sampleLrc.split('\n').map((l) => l.replace(/^\[[^\]]*\]/, '')).join('').trim()[0];
check(htmlLyric && htmlLyric.includes(firstChar),
  `HTML 谱面含歌词首字「${firstChar}」`, '歌词没进谱面');

// ---- 6. 真实录音 MIDI（量化 + 首调）也能走通 ----
c.setMidi(humanMidi);
c.setLyrics('');
const humanText = c.call('midi2jianpu_parse', -1, 1);
check(humanText && /1=C/.test(humanText), '真实录音 MIDI 识别为 a 小调（记作 1=C）', humanText);
const humanXml = c.call('midi2jianpu_musicxml', -1, 1);
check(humanXml && humanXml.includes('<attributes>'), '真实录音 MIDI 的 MusicXML 含 attributes');

// ---- 7. 播放用音符 JSON ----
const notesJson = c.call('midi2jianpu_get_notes_json', -1, 1);
let notes = null;
try {
  notes = JSON.parse(notesJson || 'null');
} catch (e) {
  /* 留给下面的断言报错 */
}
check(Array.isArray(notes) && notes.length > 0, '音符 JSON 合法且非空', notesJson && notesJson.slice(0, 120));
check(notes && notes.length && typeof notes[0].time === 'number',
  '音符 JSON 字段 time/pitch 完整', notes && JSON.stringify(notes[0]));

// ---- 8. 坏数据不能把整页搞崩 ----
c.setMidi(new Uint8Array([1, 2, 3, 4, 5]));
const bad = c.call('midi2jianpu_parse', -1, 1);
check(typeof bad === 'string' && bad.length > 0, '非法 MIDI 返回错误文本而非崩溃', bad);
const badXml = c.call('midi2jianpu_musicxml', -1, 1);
check(typeof badXml === 'string' && badXml.includes('<error>'), '非法 MIDI 的 MusicXML 返回 error 节点', badXml);

console.log('');
if (failures.length) {
  console.log(`X ${failures.length} 项失败`);
  process.exit(1);
}
console.log('全部通过');
