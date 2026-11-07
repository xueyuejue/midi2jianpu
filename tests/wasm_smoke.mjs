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
  let useMidiLyrics = 1;

  function setMidi(bytes) {
    midi = bytes;
  }
  function setLyrics(txt) {
    lyrics = txt || '';
  }
  function setTitle(txt) {
    title = txt || '';
  }
  function setUseMidiLyrics(on) {
    useMidiLyrics = on ? 1 : 0;
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

    // 内置歌词开关也是全局状态，网页每次转换都重设，这里保持一致
    if (mod._midi2jianpu_set_use_midi_lyrics) {
      mod._midi2jianpu_set_use_midi_lyrics(useMidiLyrics);
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
  return { setMidi, setLyrics, setTitle, setUseMidiLyrics, call };
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
  '_midi2jianpu_key_change_count', // 网页的转调提示
  '_midi2jianpu_tempo_change_count', // 网页的「LRC 已按变速分段对齐」提示
  '_midi2jianpu_set_use_pentatonic', // 网页的五声校正开关
  '_midi2jianpu_pentatonic_info', // 网页的调式提示
  '_midi2jianpu_set_use_dynamics', // 网页的「力度与演奏法」开关
  '_midi2jianpu_expression_count', // 网页的表情检出提示
  '_midi2jianpu_part_info', // 网页的声部乐器标注提示
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

// ---- 10. 弱起小节（网页侧同样要按新网格划小节线）----
// 弱起曲目的第一小节不满四拍，网页版与 CLI 共用 BarGrid，若只改了 CLI，
// 网页导出的小节号与和弦仍会整体错一拍。
const pickupPath = path.join(FIXTURES, 'pickup.mid');
if (existsSync(pickupPath)) {
  const p = makeCaller(mod);
  p.setMidi(readFileSync(pickupPath));
  p.setLyrics('');
  const pText = p.call('midi2jianpu_parse', -1, 1);
  check(pText && pText.includes('弱起 1 拍'), '文本谱头标注弱起', pText && pText.slice(0, 200));
  check(pText && pText.includes("[G] 5  |"), '弱起小节的和弦是 G', pText && pText.slice(0, 300));
  const pHtml = p.call('midi2jianpu_html', -1, 1);
  check(pHtml && pHtml.includes('弱起 1 拍'), 'HTML 页眉标注弱起');
  check(pHtml && (pHtml.match(/class='barno'>1</g) || []).length === 2,
    '两条轨的第一个完整小节都编号为 1（弱起小节不编号）');
  const pXml = p.call('midi2jianpu_musicxml', -1, 1);
  check(pXml && pXml.includes('number="0" implicit="yes"'),
    'MusicXML 把弱起小节标成 implicit', pXml && pXml.slice(0, 300));
} else {
  check(false, '素材 pickup.mid 存在（先跑 make_fixture.py）');
}

// ---- 11. 一字多音：网页 HTML 弧线 + 网页导出的 MusicXML 歌词 ----
// 网页版不经过 main.cpp，若只给 CLI 接上歌词，用户下载 MusicXML 仍然没词。
const melismaPath = path.join(FIXTURES, 'melisma.mid');
if (existsSync(melismaPath)) {
  const m = makeCaller(mod);
  m.setMidi(readFileSync(melismaPath));
  m.setLyrics('[00:00.00]一二三四\n[00:04.00]五六七八\n');
  const mHtml = m.call('midi2jianpu_html', -1, 1);
  check(mHtml && (mHtml.match(/class='sgrp'/g) || []).length === 4,
    'HTML 给四个字各画一条连音线弧线');
  check(mHtml && mHtml.includes("class='staff hasslur'"), '连音线轨抬高和弦带');
  const mXml = m.call('midi2jianpu_musicxml', -1, 1);
  check(mXml && mXml.includes('<lyric') && mXml.includes('<text>一</text>'),
    '网页导出的 MusicXML 带歌词');
  check(mXml && (mXml.match(/<slur type="start"/g) || []).length === 4,
    '网页导出的 MusicXML 有 4 个连音线起点');
} else {
  check(false, '素材 melisma.mid 存在（先跑 make_fixture.py）');
}

// ---- 12. MIDI 内置歌词：不贴 LRC 也该有词 ----
// 网页版不经过 main.cpp，且用户可能根本没打开歌词面板。上一版只认
// set_lyrics 传进来的 LRC，自带歌词的 .mid 打开后是空白谱面。
const karaokePath = path.join(FIXTURES, 'karaoke.mid');
if (existsSync(karaokePath)) {
  const k = makeCaller(mod);
  const kb = readFileSync(karaokePath);
  k.setMidi(kb);
  k.setLyrics('');                 // 刻意不提供 LRC

  const kHtml = k.call('midi2jianpu_html', -1, 1);
  check(kHtml && kHtml.includes("<span class='lyric'>你</span>"),
    '网页版自动显示 MIDI 内置歌词', kHtml && kHtml.slice(0, 200));
  check(kHtml && kHtml.includes('歌词：MIDI 内置（5 音节）'),
    '页眉标注歌词来源');
  check(kHtml && kHtml.includes("<span class='lyric'>Hello</span>"),
    '英文单词按一个音节显示，未被拆成字母');
  check(kHtml && (kHtml.match(/class='sgrp'/g) || []).length === 3,
    '3 条连音线弧线');

  const kXml = k.call('midi2jianpu_musicxml', -1, 1);
  check(kXml && kXml.includes('<text>Hello</text>'),
    '网页导出的 MusicXML 带内置歌词');
  check(kXml && (kXml.match(/<slur type="start"/g) || []).length === 3,
    '网页导出的 MusicXML 有 3 个连音线起点');

  // 网页用来提示「无需另找 LRC」的查询接口
  const qp = mod._malloc(kb.length);
  mod.HEAPU8.set(kb, qp);
  const cnt = mod._midi2jianpu_midi_lyric_count
    ? mod._midi2jianpu_midi_lyric_count(qp, kb.length) : -1;
  mod._free(qp);
  check(cnt === 5, '内置歌词音节数查询接口返回 5', String(cnt));

  k.setUseMidiLyrics(false);
  const kOff = k.call('midi2jianpu_html', -1, 1);
  check(kOff && !kOff.includes("<span class='lyric'>你</span>"),
    '关闭内置歌词开关后谱面无词');
  check(kOff && !kOff.includes('歌词：MIDI 内置'),
    '关闭后页眉也不再标注来源');
  k.setUseMidiLyrics(true);

  k.setLyrics('[00:00.00]甲乙\n');
  const kExt = k.call('midi2jianpu_html', -1, 1);
  check(kExt && kExt.includes('歌词：外部 LRC')
    && !kExt.includes("<span class='lyric'>你</span>"),
    '粘贴 LRC 后优先使用外部歌词');
} else {
  check(false, '素材 karaoke.mid 存在（先跑 make_fixture.py）');
}

// ---- 13. 中途变拍号：WASM 产物必须带上新的拍号 map ----
const meterPath = path.join(FIXTURES, 'meter.mid');
if (existsSync(meterPath)) {
  const mc = makeCaller(mod);
  mc.setMidi(new Uint8Array(readFileSync(meterPath)));
  mc.setTitle('拍号测试');
  const mh = mc.call('midi2jianpu_html', -1, 1);
  check(mh && mh.includes('4/4 → 3/4(第3小节) → 6/8(第5小节)'),
    'WASM 版页眉写出拍号摘要（旧产物只会写最后一个 6/8）');
  check(mh && (mh.match(/class='meter'>/g) || []).length === 2
    && mh.includes("class='meter'>3/4") && mh.includes("class='meter'>6/8"),
    'WASM 版谱面标出两个新拍号');
  check(mh && !mh.includes('弱起'), 'WASM 版不再误判弱起');
  check(mh && mh.includes('歌词：MIDI 内置（24 音节）')
    && (mh.match(/class='sgrp'/g) || []).length === 2,
    'WASM 版变拍号下内置歌词与连音线仍正确');
  check(mh && mh.includes('已跳过鼓轨'), 'WASM 版仍能提示鼓轨被丢弃');

  const mx = mc.call('midi2jianpu_musicxml', -1, 1);
  check(mx && mx.includes('<beats>3</beats>') && mx.includes('<beats>6</beats>')
    && mx.includes('<beat-type>8</beat-type>'),
    'WASM 版 MusicXML 在变拍号小节写出新的 <time>');
  check(mx && mx.includes('<lyric'), 'WASM 版 MusicXML 仍带内置歌词');

  mc.setMidi(new Uint8Array(sampleMidi));
  const mPlain = mc.call('midi2jianpu_html', -1, 1);
  check(mPlain && !mPlain.includes("class='meter'>"), 'WASM 版单拍号素材不出现拍号标签');
} else {
  check(false, '素材 meter.mid 存在（先跑 make_fixture.py）');
}

// ---- 14. 中途转调：WASM 产物必须带上调号 map 与转调点查询 ----
const modulationPath = path.join(FIXTURES, 'modulation.mid');
if (existsSync(modulationPath)) {
  const modBytes = new Uint8Array(readFileSync(modulationPath));
  const qc = mod._malloc(modBytes.length);
  mod.HEAPU8.set(modBytes, qc);
  const kc = mod._midi2jianpu_key_change_count
    ? mod._midi2jianpu_key_change_count(qc, modBytes.length) : -1;
  mod._free(qc);
  check(kc === 2, '转调点查询接口返回 2（网页据此提示按段换算）', String(kc));

  const kc2 = makeCaller(mod);
  kc2.setMidi(modBytes);
  kc2.setTitle('转调测试');
  const kh = kc2.call('midi2jianpu_html', -1, 1);
  check(kh && kh.includes('1=C 大调 → 1=D(第5小节) → 1=F(第9小节)'),
    'WASM 版页眉写出完整转调链（旧产物只会写最后一个调号）');
  check(kh && (kh.match(/class='keychg'>/g) || []).length === 4
    && kh.includes("class='keychg'>转 1=D"),
    'WASM 版谱面在两条轨各标出 2 处转调');
  check(kh && kh.includes('歌词：MIDI 内置（46 音节）')
    && (kh.match(/class='sgrp'/g) || []).length === 2,
    'WASM 版转调下内置歌词与连音线仍正确');

  const kt = kc2.call('midi2jianpu_parse', -1, 1);
  check(kt && kt.includes('1 2 3 4') && kt.includes('#6, 1 2 3')
    && kt.includes('5, 1 2 3') && kt.includes('[转1=D]'),
    'WASM 版同一个 C4 在三段唱名不同（旧产物全曲按 F 大调算）');

  const kx = kc2.call('midi2jianpu_musicxml', -1, 1);
  const kf = kx ? [...kx.matchAll(/<fifths>(-?\d+)<\/fifths>/g)].map(m => m[1]) : [];
  check(kf.length === 6 && kf.filter(v => v === '0').length === 2
    && kf.filter(v => v === '2').length === 2 && kf.filter(v => v === '-1').length === 2,
    'WASM 版 MusicXML 每个声部写出 C / D / F 三个调号', String(kf));

  // 网页下拉框选调时，转调链要整体平移，不能跳回 MIDI 的绝对调号
  const kg = kc2.call('midi2jianpu_html', 7, 1);
  check(kg && kg.includes('1=G 大调 → 1=A(第5小节) → 1=C(第9小节)'),
    'WASM 版手动选 G 调后转调链整体平移');

  const sq = mod._malloc(sampleMidi.length);
  mod.HEAPU8.set(sampleMidi, sq);
  const zero = mod._midi2jianpu_key_change_count(sq, sampleMidi.length);
  mod._free(sq);
  check(zero === 0, '单调号素材的转调点查询返回 0', String(zero));
} else {
  check(false, '素材 modulation.mid 存在（先跑 make_fixture.py）');
}

// ---- 15. 五声调式：宫音校正 + 调式查询 + 网页开关 ----
const pentPath = path.join(FIXTURES, 'pentatonic.mid');
if (existsSync(pentPath)) {
  const pentBytes = new Uint8Array(readFileSync(pentPath));
  const pq = mod._malloc(pentBytes.length);
  mod.HEAPU8.set(pentBytes, pq);
  const info = mod._midi2jianpu_pentatonic_info(pq, pentBytes.length);
  mod._free(pq);
  // final=G(7) * 100 + gong=C(0) * 10 + 徵(5) = 705
  check(info === 705, '调式查询接口打包出 G 徵调式 / 宫音 C', String(info));

  const sq2 = mod._malloc(sampleMidi.length);
  mod.HEAPU8.set(sampleMidi, sq2);
  const sigInfo = mod._midi2jianpu_pentatonic_info(sq2, sampleMidi.length);
  mod._free(sq2);
  check(sigInfo === 0, '自带 0x59 调号的文件不报五声校正（以调号为准）', String(sigInfo));

  const pc = makeCaller(mod);
  pc.setMidi(pentBytes);
  pc.setTitle('五声调式测试');
  const ph = pc.call('midi2jianpu_html', -1, 1);
  check(ph && ph.includes('1=C 大调 · G 徵调式（宫音 C）'),
    'WASM 版按宫音校正为 1=C 并写出调式名');
  check(ph && ph.includes('五声宫音校正'), 'WASM 版页眉标明识别来源');
  const pt = pc.call('midi2jianpu_parse', -1, 1);
  check(pt && pt.includes('[C] 1 2 3 5'), 'WASM 版 G 记作 5 而不是 1', pt);
  const px = pc.call('midi2jianpu_musicxml', -1, 1);
  check(px && px.includes('<words>G 徵调式（宫音 C）</words>')
    && px.includes('<fifths>0</fifths>'),
    'WASM 版 MusicXML 写调式文字标记与 C 大调调号');

  // 网页复选框关掉：回到 KS 的 1=G，查询接口也归零
  mod._midi2jianpu_set_use_pentatonic(0);
  const pOff = pc.call('midi2jianpu_html', -1, 1);
  check(pOff && pOff.includes('1=G 大调') && !pOff.includes('G 徵调式'),
    'WASM 版关闭五声校正后回到 1=G', (pOff || '').slice(0, 400));
  const offQ = mod._malloc(pentBytes.length);
  mod.HEAPU8.set(pentBytes, offQ);
  check(mod._midi2jianpu_pentatonic_info(offQ, pentBytes.length) === 0,
    '关闭开关后调式查询返回 0');
  mod._free(offQ);
  mod._midi2jianpu_set_use_pentatonic(1);
  check(pc.call('midi2jianpu_html', -1, 1).includes('1=C 大调 · G 徵调式'),
    '重新打开开关后恢复校正');
} else {
  check(false, '素材 pentatonic.mid 存在（先跑 make_fixture.py）');
}

// ---- 16. LRC 时间轴 × tempo map（网页版走的是同一套 C++ 解析器）----
const tlPath = path.join(FIXTURES, 'tempolrc.mid');
const tlLrcPath = path.join(FIXTURES, 'tempolrc.lrc');
if (existsSync(tlPath) && existsSync(tlLrcPath)) {
  const tlBytes = new Uint8Array(readFileSync(tlPath));
  const tlLrc = readFileSync(tlLrcPath, 'utf8');
  const tlOffLrc = readFileSync(path.join(FIXTURES, 'tempolrc_offset.lrc'), 'utf8');
  const expect = '春风又绿江南岸边明月何时照我还乡小楼昨夜又东风起故国不堪回首月中';

  const tq = mod._malloc(tlBytes.length);
  mod.HEAPU8.set(tlBytes, tq);
  check(mod._midi2jianpu_tempo_change_count(tq, tlBytes.length) === 1,
    '变速点查询返回 1 处（120 -> 60 BPM）');
  mod._free(tq);
  const sq0 = mod._malloc(sampleMidi.length);
  mod.HEAPU8.set(sampleMidi, sq0);
  check(mod._midi2jianpu_tempo_change_count(sq0, sampleMidi.length) === 0,
    '单调速素材不报变速');
  mod._free(sq0);

  const tc = makeCaller(mod);
  tc.setMidi(tlBytes);
  tc.setLyrics(tlLrc);
  tc.setTitle('变速歌词测试');
  const th = tc.call('midi2jianpu_html', -1, 1);
  const htmlWords = th ? [...th.matchAll(/class='lyric'>([^<]*)</g)].map(m => m[1]).join('') : '';
  check(htmlWords === expect,
    'WASM HTML 逐字写出 32 个歌词字（后半段没跑偏）', htmlWords);

  const tx = tc.call('midi2jianpu_musicxml', -1, 1);
  const xmlWords = tx ? [...tx.matchAll(/<text>(.*?)<\/text>/g)].map(m => m[1]).join('') : '';
  check(xmlWords === expect,
    'WASM MusicXML 的 <lyric> 也是完整 32 字', xmlWords);

  tc.setLyrics(tlOffLrc);
  const thOff = tc.call('midi2jianpu_html', -1, 1);
  const offWords = thOff ? [...thOff.matchAll(/class='lyric'>([^<]*)</g)].map(m => m[1]).join('') : '';
  check(offWords === expect,
    'WASM 版 [offset:-2000] 与主 LRC 等价', offWords);
} else {
  check(false, '素材 tempolrc.mid / tempolrc.lrc 存在（先跑 make_fixture.py）');
}

// ---- 17. 坏数据不能把整页搞崩 ----
c.setMidi(new Uint8Array([1, 2, 3, 4, 5]));
const bad = c.call('midi2jianpu_parse', -1, 1);
check(typeof bad === 'string' && bad.length > 0, '非法 MIDI 返回错误文本而非崩溃', bad);
const badXml = c.call('midi2jianpu_musicxml', -1, 1);
check(typeof badXml === 'string' && badXml.includes('<error>'), '非法 MIDI 的 MusicXML 返回 error 节点', badXml);

// ---- 18. 演奏表情：网页开关、计数查询与三层输出 ----
const exPath = path.join(FIXTURES, 'expression.mid');
if (existsSync(exPath)) {
  const exBytes = new Uint8Array(readFileSync(exPath));
  const ep = mod._malloc(exBytes.length);
  mod.HEAPU8.set(exBytes, ep);
  check(mod._midi2jianpu_expression_count(ep, exBytes.length) === 24,
    '表情计数查询返回 24 处（11 力度 + 2 渐变 + 6 演奏法 + 3 弯音 + 2 段落）',
    mod._midi2jianpu_expression_count(ep, exBytes.length));
  mod._midi2jianpu_set_use_dynamics(0);
  check(mod._midi2jianpu_expression_count(ep, exBytes.length) === 0,
    '关掉开关后表情计数返回 0（网页提示随之消失）');
  mod._midi2jianpu_set_use_dynamics(1);
  mod._free(ep);

  const ec = makeCaller(mod);
  ec.setMidi(exBytes);
  ec.setTitle('表情测试');
  const eh = ec.call('midi2jianpu_html', -1, 1) || '';
  check(eh.includes("class='section'>Verse") && eh.includes("class='wedge'>cresc.") &&
        eh.includes("class='artic'>↗"), 'WASM HTML 写出段落名 / 渐强 / 滑音');
  const exml = ec.call('midi2jianpu_musicxml', -1, 1) || '';
  check(exml.includes('<staccatissimo/>') &&
        exml.includes('<wedge type="crescendo"') &&
        exml.includes('<trill-mark/>'), 'WASM MusicXML 写出断奏 / 渐强 / 颤音');
  mod._midi2jianpu_set_use_dynamics(0);
  const eoff = ec.call('midi2jianpu_html', -1, 1) || '';
  check(!eoff.includes("class='section'>") && !eoff.includes("class='artic'>"),
    'WASM 关掉开关后 HTML 无表情标记');
  mod._midi2jianpu_set_use_dynamics(1);

  const nj = ec.call('midi2jianpu_get_notes_json', -1, 1);
  const n0 = JSON.parse(nj || '[]');
  check(Array.isArray(n0) && n0.length > 0 && typeof n0[0].velocity === 'number',
    '播放 JSON 带 note-on velocity（网页据此区分强弱）',
    nj && nj.slice(0, 120));
} else {
  check(false, '素材 expression.mid 存在（先跑 make_fixture.py）');
}

// ---- 19. 声部乐器标注查询（网页据此在文件面板提示声部构成）----
// 这是 v1.21.0 新增的导出；旧 wasm 产物缺它，网页「声部」提示会拿不到数据。
const partsPath = path.join(FIXTURES, 'parts.mid');
if (existsSync(partsPath)) {
  const pb = new Uint8Array(readFileSync(partsPath));
  const pq = mod._malloc(pb.length);
  mod.HEAPU8.set(pb, pq);
  const pinfo = mod._midi2jianpu_part_info
    ? mod.UTF8ToString(mod._midi2jianpu_part_info(pq, pb.length)) : null;
  mod._free(pq);
  check(typeof pinfo === 'string' && pinfo.includes('声部：')
    && pinfo.includes('Vocal·长笛') && pinfo.includes('Piano·钢琴')
    && pinfo.includes('Bass·原声贝斯'),
    'part_info 返回各声部与 GM 乐器中文名', pinfo);

  // 同样的查询接口对普通（带 0xC0）曲子也能稳定返回字符串（不崩溃）
  const sq3 = mod._malloc(sampleMidi.length);
  mod.HEAPU8.set(sampleMidi, sq3);
  const sInfo = mod.UTF8ToString(mod._midi2jianpu_part_info(sq3, sampleMidi.length));
  mod._free(sq3);
  check(typeof sInfo === 'string' && sInfo.length > 0,
    'part_info 对任意曲子返回非空字符串', sInfo);
  // 无 0xC0 的曲子不强行拼乐器名这一点由单元测试的构造样例锁死；
  // 这里只验证接口稳健、不会返回 null 或抛异常。
} else {
  check(false, '素材 parts.mid 存在（先跑 make_fixture.py）');
}

// ---- 20. 和弦转位 / 七和弦：网页与 MusicXML 导出必须同步 ----
// 和弦检测在 C++ 里改一次，网页和导出就都得跟着变；这里用 chords.mid 锁住
// C/E（转位）与 Bm7b5（半减七），避免出现「CLI 有斜杠、网页却没有」的半截状态。
const chordsPath = path.join(FIXTURES, 'chords.mid');
if (existsSync(chordsPath)) {
  const cb = new Uint8Array(readFileSync(chordsPath));
  const cq = makeCaller(mod);
  cq.setMidi(cb);
  const ch = cq.call('midi2jianpu_html', -1, 1) || '';
  check(ch.includes('C/E') && ch.includes('Bm7b5') && ch.includes('G7sus4'),
    'WASM HTML 写出转位 C/E 与扩充和弦', ch.slice(0, 200));
  const cxml = cq.call('midi2jianpu_musicxml', -1, 1) || '';
  check(cxml.includes('<harmony>') && cxml.includes('<bass-step>E</bass-step>') &&
        cxml.includes('text="m7b5"'),
    'WASM MusicXML 写出 <harmony> 与转位低音', cxml.slice(0, 300));
} else {
  check(false, '素材 chords.mid 存在（先跑 make_fixture.py）');
}

console.log('');
if (failures.length) {
  console.log(`X ${failures.length} 项失败`);
  process.exit(1);
}
console.log('全部通过');
