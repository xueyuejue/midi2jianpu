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
  let title = '';

  function setMidi(bytes) {
    midi = bytes;
  }
  function setLyrics(txt) {
    lyrics = txt || '';
  }
  function setTitle(txt) {
    title = txt || '';
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

    if (mod._midi2jianpu_set_title) {
      const n = mod.lengthBytesUTF8(title) + 1;
      const tp = mod._malloc(n);
      mod.stringToUTF8(title, tp, n);
      mod._midi2jianpu_set_title(tp);
      mod._free(tp);
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
  return { setMidi, setLyrics, setTitle, call };
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
check(html && html.includes('class=\'bgrp'), 'HTML 含连成一体的减时线组');

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
c.setTitle('蝶衣碎雪');
const htmlLyric = c.call('midi2jianpu_html', -1, 1);
const firstChar = sampleLrc.split('\n').map((l) => l.replace(/^\[[^\]]*\]/, '')).join('').trim()[0];
check(htmlLyric && htmlLyric.includes(firstChar),
  `HTML 谱面含歌词首字「${firstChar}」`, '歌词没进谱面');

// 标题应写成歌名；调号行不能再出现重复的 "1 = 1=C"
check(htmlLyric && htmlLyric.includes('<h1>蝶衣碎雪</h1>'), 'HTML 谱面标题取传入的歌名',
  htmlLyric && (htmlLyric.match(/<h1>[^<]*<\/h1>/) || [''])[0]);
check(!/1 = 1=C/.test(htmlLyric || ''), '调号行不重复写 "1 = 1=C"');
check(/<footer[^>]*>[^<]*v\d/.test(htmlLyric || ''), '页脚标注版本号');

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

// ---- 8. 打击乐轨：网页叫谱面里不能出现鼓 ----
// 鼓轨音符数远超旋律时，旧的"音符最多即旋律轨"逻辑会把鼓当旋律转出来，
// 谱面变成 `1,, #4,, #6,,` 的垃圾，和弦与歌词也跟着挂错。CLI 与网页共用
// 同一份解析代码，这里兜住网页侧。
const drumsPath = path.join(FIXTURES, 'drums.mid');
if (existsSync(drumsPath)) {
  const d = makeCaller(mod);
  d.setMidi(readFileSync(drumsPath));
  d.setLyrics('');
  const drumsText = d.call('midi2jianpu_parse', -1, 1);
  check(drumsText && /已跳过 192 个打击乐音符/.test(drumsText),
    '鼓轨音符被跳过并提示用户', drumsText && drumsText.slice(0, 200));
  check(drumsText && !/\[Drums\]/.test(drumsText), '文本谱面里没有鼓轨', drumsText);
  check(drumsText && !/1,,/.test(drumsText), '鼓的音高没有混进简谱', drumsText);

  const drumsHtml = d.call('midi2jianpu_html', -1, 1);
  check(drumsHtml && !drumsHtml.includes('Drums'), 'HTML 谱面里没有鼓轨');
  check(drumsHtml && drumsHtml.includes('已跳过鼓轨'), 'HTML 页眉标注已跳过鼓轨');

  // 播放用的音符也不能有鼓，否则网页会拿合成器去"弹"军鼓
  const drumsNotes = JSON.parse(d.call('midi2jianpu_get_notes_json', -1, 1) || 'null');
  check(Array.isArray(drumsNotes) && drumsNotes.length === 32,
    '播放音符只剩旋律与钢琴（32 个），实际: ' + (Array.isArray(drumsNotes) ? drumsNotes.length : 'null'));
} else {
  check(false, '素材 drums.mid 存在（先跑 make_fixture.py）');
}

// ---- 9. 和声粒度与变速标记（与 CLI 共用同一份代码，网页侧也兜一道）----
const harmonyPath = path.join(FIXTURES, 'harmony.mid');
if (existsSync(harmonyPath)) {
  const h = makeCaller(mod);
  h.setMidi(readFileSync(harmonyPath));
  h.setLyrics('');
  const hText = h.call('midi2jianpu_parse', -1, 1);
  check(hText && hText.includes("[C] 1' 7 [Am] 6 5"),
    '一小节里同时标出 C 与 Am', hText && hText.slice(0, 200));
  check(hText && hText.includes('♩=120 → 90(第5小节)'),
    '页眉写出中途变速', hText && hText.slice(0, 160));

  const hHtml = h.call('midi2jianpu_html', -1, 1);
  check(hHtml && (hHtml.match(/class='chordx'/g) || []).length === 16,
    'HTML 有 16 个和弦（8 小节 x 2），全部挂在音符上方');
  check(hHtml && (hHtml.match(/class='tempo'/g) || []).length === 2,
    'HTML 只印 2 个速度标记（每轨各印一遍会变 4 个）');
} else {
  check(false, '素材 harmony.mid 存在（先跑 make_fixture.py）');
}

// ---- 10. 坏数据不能把整页搞崩 ----
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
