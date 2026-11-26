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
  '_midi2jianpu_set_soft_pedal_scale', // 网页的软踏板强度下拉
  '_midi2jianpu_expression_count', // 网页的表情检出提示
  '_midi2jianpu_part_info', // 网页的声部乐器标注提示
  '_midi2jianpu_tempo_ramp_count', // 网页的「其中 N 处渐快渐慢」提示
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
  // final=G(7) * 10000 + gong=C(0) * 100 + 徵(5) = 70005（v1.172 两位宽编码）
  check(info === 70005, '调式查询接口打包出 G 徵调式 / 宫音 C', String(info));

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

// ---- 21. 渐快渐慢：网页谱面与 MusicXML 都只写一个 rit./accel. ----
// 一段 rit. 在 MIDI 里是几十个连续 set_tempo。谱面必须收成一个记号，
// 否则网页会被逐小节的速度采样刷屏、导出的 MusicXML 又反过来整段丢失。
const rampPath = path.join(FIXTURES, 'tempo_ramp.mid');
if (existsSync(rampPath)) {
  const rb = new Uint8Array(readFileSync(rampPath));
  const rq = makeCaller(mod);
  rq.setMidi(rb);
  const rh = rq.call('midi2jianpu_html', -1, 1) || '';
  check(rh.includes('accel. 100→140') && rh.includes('rit. 140→80'),
    'WASM HTML 把 26 个自动化节点收成 accel. / rit.', rh.slice(0, 300));
  const marks = rh.match(/<span class='tempo[^']*'>[^<]*<\/span>/g) || [];
  check(marks.length === 4, `WASM HTML 只有 4 个速度标记，实际 ${marks.length}`);

  const rxml = rq.call('midi2jianpu_musicxml', -1, 1) || '';
  check(rxml.includes('<words>rit.</words>') && rxml.includes('<words>accel.</words>'),
    'WASM MusicXML 写出 rit. / accel. 文字记号', rxml.slice(0, 300));

  // 网页提示要用折叠后的计数，不是 26
  const pcq = mod._malloc(rb.length);
  mod.HEAPU8.set(rb, pcq);
  const tc = mod._midi2jianpu_tempo_change_count(pcq, rb.length);
  const trc = mod._midi2jianpu_tempo_ramp_count(pcq, rb.length);
  mod._free(pcq);
  check(tc === 3, `变速处数按折叠后算（3 处），实际 ${tc}`);
  check(trc === 2, `其中渐快渐慢 2 处，实际 ${trc}`);
} else {
  check(false, '素材 tempo_ramp.mid 存在（先跑 make_fixture.py）');
}

// ---- 22. 长音跨小节：网页谱面与 MusicXML 都写延音线，不写休止符 ----
// long_tie.mid 的旋律轨有一个 6 拍长音和一个 10 拍长音。量化阶段曾把超过
// 全音符的时值钳到 4 拍，余下时间变成休止符——谱面上`3 - - -` 之后跟着
// `0 - 4 5`，唱到一半凭空静音两拍。
const longTiePath = path.join(FIXTURES, 'long_tie.mid');
if (existsSync(longTiePath)) {
  const lb = new Uint8Array(readFileSync(longTiePath));
  const lq = makeCaller(mod);
  lq.setMidi(lb);
  const lh = lq.call('midi2jianpu_html', -1, 1) || '';
  const dashes = (lh.match(/<span class='num'>-<\/span>/g) || []).length;
  check(dashes === 8,
    `WASM HTML 写出 8 个延音线（6 拍 2 个 + 10 拍 6 个），实际 ${dashes}`);
  check(!/>0<\/span>/.test(lh.split('Piano')[0] || ''),
    'WASM HTML 的旋律轨没有把延音写成休止符 0');

  const lxml = lq.call('midi2jianpu_musicxml', -1, 1) || '';
  const ls = (lxml.match(/<tie type="start"\/>/g) || []).length;
  const le = (lxml.match(/<tie type="stop"\/>/g) || []).length;
  check(ls === 3 && le === 3,
    `WASM MusicXML 的 <tie> 起止成对（3/3），实际 ${ls}/${le}`);
} else {
  check(false, '素材 long_tie.mid 存在（先跑 make_fixture.py）');
}

// ---- 23. 同轨复调：一条轨两个声部，网页与 MusicXML 都不让小节溢出 ----
// polyphony.mid 是钢琴左右手写进同一个轨（右手四分音符 + 左手长音）。
// 旧版把「同拍不同值」也当和弦，4/4 小节被撑成 8 拍。
const polyPath = path.join(FIXTURES, 'polyphony.mid');
if (existsSync(polyPath)) {
  const pb = new Uint8Array(readFileSync(polyPath));
  const pq = makeCaller(mod);
  pq.setMidi(pb);

  const ph = pq.call('midi2jianpu_html', -1, 1) || '';
  const staffs = (ph.match(/<div class='staff/g) || []).length;
  check(staffs === 2, `WASM HTML 拆成两块谱面（两个声部），实际 ${staffs}`);
  check(/声部 2/.test(ph), 'WASM HTML 标题标出声部号');

  const pxml = pq.call('midi2jianpu_musicxml', -1, 1) || '';
  const backups = (pxml.match(/<backup>/g) || []).length;
  check(backups === 4, `WASM MusicXML 每个复调小节各一次 <backup>，实际 ${backups}`);
  check(/<voice>2<\/voice>/.test(pxml), 'WASM MusicXML 写出 <voice>2</voice>');

  // 逐小节核对总时值（<chord/> 不推进，<backup> 回退）
  const divM = pxml.match(/<divisions>(\d+)<\/divisions>/);
  const dv = divM ? parseInt(divM[1], 10) : 480;
  const bodies = pxml.match(/<measure [^>]*>[\s\S]*?<\/measure>/g) || [];
  const overflow = bodies.filter((body) => {
    let pos = 0;
    let mx = 0;
    const frags = body.match(/<note>[\s\S]*?<\/note>|<backup>[\s\S]*?<\/backup>/g) || [];
    for (const f of frags) {
      const dm = f.match(/<duration>(\d+)<\/duration>/);
      if (!dm) continue;
      const dur = parseInt(dm[1], 10);
      if (f.startsWith('<backup>')) pos -= dur;
      else if (!f.includes('<chord/>')) { pos += dur; mx = Math.max(mx, pos); }
    }
    return mx !== 4 * dv;
  });
  check(overflow.length === 0,
    `WASM MusicXML 每个小节正好 4 拍，溢出小节 ${overflow.length} 个`);
} else {
  check(false, '素材 polyphony.mid 存在（先跑 make_fixture.py）');
}

// ---- 24. 文件头 MThd：SMPTE 时间基准与 format 2 独立序列 ----
// 这两类文件过去会被静默读错：SMPTE 的 0xE728 被当成无符号 PPQ 59176，
// format 2 的多段独立序列被并成一首。网页若不提示，用户只会觉得「转换坏了」。
const smptePath = path.join(FIXTURES, 'smpte.mid');
check(existsSync(smptePath), '素材 smpte.mid 存在（先跑 make_fixture.py）');
if (existsSync(smptePath)) {
  const sb = new Uint8Array(readFileSync(smptePath));
  const sptr = mod._malloc(sb.length);
  mod.HEAPU8.set(sb, sptr);
  const fh = mod.UTF8ToString(mod._midi2jianpu_file_header_info(sptr, sb.length));
  mod._free(sptr);
  check(/SMPTE 25 fps/.test(fh) && /500 tick/.test(fh),
    `WASM 报出 SMPTE 折算结果，实际：${fh}`);

  const sc = makeCaller(mod);
  sc.setMidi(sb);
  const sh = sc.call('midi2jianpu_html', -1, 1) || '';
  check(sh.includes('SMPTE 25 fps'), 'WASM HTML 页眉写有时间基准');

  const sx = sc.call('midi2jianpu_musicxml', -1, 1) || '';
  const sd = sx.match(/<divisions>(\d+)<\/divisions>/);
  check(sd && sd[1] === '500',
    `WASM MusicXML divisions 用折算后的 500，实际 ${sd && sd[1]}`);
  const smeasures = (sx.match(/<measure /g) || []).length;
  check(smeasures === 4, `WASM MusicXML 四个小节，实际 ${smeasures}`);
}

// 曲首同 tick 两条 set_tempo（60 先写、120 后写）：网页与 CLI 共用解析层，
// 必须同样取后写覆盖；旧版折出 1000 tick/拍，把四分音印成八分音。
const st0Path = path.join(FIXTURES, 'smpte_tempo0.mid');
check(existsSync(st0Path), '素材 smpte_tempo0.mid 存在（先跑 make_fixture.py）');
if (existsSync(st0Path)) {
  const sb2 = new Uint8Array(readFileSync(st0Path));
  const sp2 = mod._malloc(sb2.length);
  mod.HEAPU8.set(sb2, sp2);
  const fh2 = mod.UTF8ToString(mod._midi2jianpu_file_header_info(sp2, sb2.length));
  mod._free(sp2);
  check(/SMPTE 25 fps/.test(fh2) && /500 tick/.test(fh2) && /♩=120/.test(fh2),
    `WASM 按后写的 120BPM 折算 division，实际：${fh2}`);

  const c2 = makeCaller(mod);
  c2.setMidi(sb2);
  const sx2 = c2.call('midi2jianpu_musicxml', -1, 1) || '';
  const sd2 = sx2.match(/<divisions>(\d+)<\/divisions>/);
  check(sd2 && sd2[1] === '500',
    `WASM MusicXML divisions 用折算后的 500，实际 ${sd2 && sd2[1]}`);
  check(sx2.includes('<sound tempo="120"/>'), 'WASM MusicXML 谱头速度取后写的 120');
  check((sx2.match(/<type>quarter<\/type>/g) || []).length === 4 &&
        !sx2.includes('<type>eighth</type>') && !sx2.includes('<type>half</type>'),
    'WASM 四个 500 tick 的音都是四分音符（旧版印成八分）');
}

// 合法 MIDI 音高 0 是 C-1，不是休止符：网页版共用 html_writer，旧版把
// 落在 pitch 0 上的歌词吞掉，MusicXML 却照写。
const p0Path = path.join(FIXTURES, 'pitch0.mid');
const p0LrcPath = path.join(FIXTURES, 'pitch0.lrc');
check(existsSync(p0Path) && existsSync(p0LrcPath),
  '素材 pitch0.mid/.lrc 存在（先跑 make_fixture.py）');
if (existsSync(p0Path) && existsSync(p0LrcPath)) {
  const p0Bytes = new Uint8Array(readFileSync(p0Path));
  const cP0 = makeCaller(mod);
  cP0.setMidi(p0Bytes);
  cP0.setLyrics(readFileSync(p0LrcPath, 'utf8'));
  const p0Html = cP0.call('midi2jianpu_html', -1, 1) || '';
  const p0Xml = cP0.call('midi2jianpu_musicxml', -1, 1) || '';
  const p0Words = [...p0Html.matchAll(/class='lyric'>([^<]*)</g)]
    .map((m) => m[1]).filter((w) => w.trim()).join('');
  const p0XmlWords = [...p0Xml.matchAll(/<text>([^<]*)<\/text>/g)]
    .map((m) => m[1]).join('');
  check(p0Words === '甲乙丙丁' && p0XmlWords === '甲乙丙丁',
    'WASM 的 pitch 0 音符照常挂词，HTML 与 MusicXML 都是甲乙丙丁',
    `html=${p0Words} xml=${p0XmlWords}`);
  check(p0Xml.includes('<pitch><step>C</step><octave>-1</octave></pitch>'),
    'WASM MusicXML 把 pitch 0 写成 C-1 真音');
}

const f2Path = path.join(FIXTURES, 'format2.mid');
check(existsSync(f2Path), '素材 format2.mid 存在（先跑 make_fixture.py）');
if (existsSync(f2Path)) {
  const fb = new Uint8Array(readFileSync(f2Path));
  const fptr = mod._malloc(fb.length);
  mod.HEAPU8.set(fb, fptr);
  const finfo = mod.UTF8ToString(mod._midi2jianpu_file_header_info(fptr, fb.length));
  mod._free(fptr);
  check(/format 2/.test(finfo) && /2 段/.test(finfo),
    `WASM 说明 format 2 只转了第一段，实际：${finfo}`);

  const fc = makeCaller(mod);
  fc.setMidi(fb);
  const fhtml = fc.call('midi2jianpu_html', -1, 1) || '';
  check(fhtml.includes('SongA') && !fhtml.includes('SongB'),
    'WASM HTML 只有 SongA，另一段没混进来');
  const fxml = fc.call('midi2jianpu_musicxml', -1, 1) || '';
  check((fxml.match(/<measure /g) || []).length === 4,
    `WASM MusicXML 只写 SongA 的 4 小节，实际 ${(fxml.match(/<measure /g) || []).length}`);
}

// 网页脚本里两处返回 char* 的调用必须过 UTF8ToString。少包一层就会在上传
// 文件时抛 TypeError（拿指针当字符串用），连带 convert() 都不执行。
const pageHtml = readFileSync(path.join(ROOT, 'docs', 'index.html'), 'utf8');
check(/UTF8ToString\(\s*wasmMod\._midi2jianpu_part_info/.test(pageHtml),
  'docs/index.html 的 part_info 用 UTF8ToString 取了字符串');
check(/UTF8ToString\(\s*wasmMod\._midi2jianpu_file_header_info/.test(pageHtml),
  'docs/index.html 的 file_header_info 用 UTF8ToString 取了字符串');

// ---- 25. 事件流对齐与音符配对：0xA0 / 重触发 / 悬挂音 / 双歌词轨 ----
// 这四个都是「文件没按理想写法写」就悄悄出错的情形，网页端必须和 CLI 同步修好，
// 否则用户只会看到「未提取到音符」或者半截歌词。
function readFixture(name) {
  const p = path.join(FIXTURES, name);
  check(existsSync(p), `素材 ${name} 存在（先跑 make_fixture.py）`);
  return existsSync(p) ? new Uint8Array(readFileSync(p)) : null;
}

const atBytes = readFixture('aftertouch.mid');
if (atBytes) {
  const ac = makeCaller(mod);
  ac.setMidi(atBytes);
  const ahtml = ac.call('midi2jianpu_html', -1, 1) || '';
  check(!ahtml.includes('未提取到音符'),
    '0xA0 复音触后不再把整轨读崩（网页不再报「未提取到音符」）');
  const axml = ac.call('midi2jianpu_musicxml', -1, 1) || '';
  const am = (axml.match(/<measure /g) || []).length;
  check(am === 4, `WASM MusicXML 四个小节，实际 ${am}`);
  const anotes = (axml.match(/<note>/g) || []).length;
  check(anotes === 16, `WASM MusicXML 16 个音符，实际 ${anotes}`);
}

const rtBytes = readFixture('retrigger.mid');
if (rtBytes) {
  const rc = makeCaller(mod);
  rc.setMidi(rtBytes);
  const rxml = rc.call('midi2jianpu_musicxml', -1, 1) || '';
  const rnotes = (rxml.match(/<note>/g) || []).length;
  check(rnotes === 8, `同音重触发不吞音，8 个音符，实际 ${rnotes}`);
  const rc4 = (rxml.match(/<step>C<\/step><octave>4<\/octave>/g) || []).length;
  check(rc4 === 2, `两个 C4 都在，实际 ${rc4} 个`);
}

const dgBytes = readFixture('dangling.mid');
if (dgBytes) {
  const dc = makeCaller(mod);
  dc.setMidi(dgBytes);
  const dxml = dc.call('midi2jianpu_musicxml', -1, 1) || '';
  const dnotes = (dxml.match(/<note>/g) || []).length;
  check(dnotes === 16, `轨尾悬挂音补收，16 个音符，实际 ${dnotes}`);
  const dhtml = dc.call('midi2jianpu_html', -1, 1) || '';
  check(dhtml.includes('1=C 大调'), '少一个音不再把调性带偏成 F / D 小调');
}

const tlBytes = readFixture('twolyrics.mid');
if (tlBytes) {
  const tptr = mod._malloc(tlBytes.length);
  mod.HEAPU8.set(tlBytes, tptr);
  const n = mod._midi2jianpu_midi_lyric_count(tptr, tlBytes.length);
  mod._free(tptr);
  check(n === 10, `双歌词轨只算一条，10 个音节，实际 ${n}`);

  const tc = makeCaller(mod);
  tc.setMidi(tlBytes);
  const thtml = tc.call('midi2jianpu_html', -1, 1) || '';
  const words = (thtml.match(/<span class='lyric'>([^<]*)<\/span>/g) || [])
    .map((w) => w.replace(/<[^>]*>/g, '')).filter(Boolean).join('');
  check(words === '春眠不觉晓处处闻', `网页歌词不再两段交错，实际：${words}`);
  check(thtml.includes('另 1 轨的 10 个音节已忽略'), '网页页眉说明忽略了另一条歌词轨');
}

// ---- 26. 控制器语义提示：网页能读到踏板 / 清音 / CC7 改了什么 ----
// 这三项都在用户看不见的地方改谱面（时值变长、幽灵音消失、多出力度记号），
// 网页文件面板若不说一句，用户只会觉得「谱和 DAW 里对不上」。
function controllerInfo(bytes) {
  const ptr = mod._malloc(bytes.length);
  mod.HEAPU8.set(bytes, ptr);
  const s = mod.UTF8ToString(mod._midi2jianpu_controller_info(ptr, bytes.length));
  mod._free(ptr);
  return s || '';
}

const pedBytes = readFixture('pedal.mid');
if (pedBytes) {
  check(/延音踏板：补长 6 个音/.test(controllerInfo(pedBytes)),
    `WASM 报出踏板补长 6 个音，实际：${controllerInfo(pedBytes)}`);
}
const aoBytes = readFixture('allnotesoff.mid');
if (aoBytes) {
  check(/强制清音：截尾 1 个音/.test(controllerInfo(aoBytes)),
    `WASM 报出清音截尾，实际：${controllerInfo(aoBytes)}`);
}
const c7Bytes = readFixture('cc7volume.mid');
if (c7Bytes) {
  check(/CC7 主音量：补入 8 个力度点/.test(controllerInfo(c7Bytes)),
    `WASM 报出 CC7 力度点，实际：${controllerInfo(c7Bytes)}`);
}
const plainBytes = readFixture('sample.mid');
if (plainBytes) {
  check(controllerInfo(plainBytes) === '',
    `普通 PPQ 文件不输出控制器提示，实际：${controllerInfo(plainBytes)}`);
}

// ---- 27. 控制器语义在 WASM 三个输出层同步生效 ----
if (pedBytes) {
  const pc = makeCaller(mod);
  pc.setMidi(pedBytes);
  const phtml = pc.call('midi2jianpu_html', -1, 1) || '';
  check(phtml.includes('延音踏板：补长 6 个音'),
    '网页 HTML 页眉写明踏板补长');
  check(!phtml.includes('1= 3= 5='), '网页谱面不再是一串十六分音符');
  const pxml = pc.call('midi2jianpu_musicxml', -1, 1) || '';
  const pdurs = (pxml.match(/<duration>1920<\/duration>/g) || []).length;
  check(pdurs >= 6, `WASM MusicXML 六个音都补成 4 拍，实际 ${pdurs} 个 1920`);
}
if (aoBytes) {
  const ac = makeCaller(mod);
  ac.setMidi(aoBytes);
  const axml = ac.call('midi2jianpu_musicxml', -1, 1) || '';
  const anotes = (axml.match(/<note>/g) || []).length;
  check(anotes >= 7, `WASM MusicXML 含 7 个真实音符，实际 ${anotes}`);
  check(!axml.includes('声部2'), '清音后不再拆出假声部');
}
if (c7Bytes) {
  const cc = makeCaller(mod);
  cc.setMidi(c7Bytes);
  const chtml = cc.call('midi2jianpu_html', -1, 1) || '';
  // HTML 用的是 <span class='dyn'>p</span> / <span class='wedge'>cresc.</span>，
  // 不是 CLI 文本里的方括号写法。
  check(/<span class='dyn'>p<\/span>/.test(chtml) &&
        /<span class='dyn'>mf<\/span>/.test(chtml) &&
        /<span class='wedge'>cresc\.<\/span>/.test(chtml),
    '网页谱面印出 CC7 带来的 p / mf / cresc.');
  check(chtml.includes('CC7 主音量：补入 8 个力度点'),
    '网页 HTML 页眉写明 CC7 力度点');
}
const pchBytes = readFixture('pedal_channels.mid');
if (pchBytes) {
  check(/延音踏板：补长 1 个音/.test(controllerInfo(pchBytes)),
    `WASM 踏板按通道隔离（只补 1 个音），实际：${controllerInfo(pchBytes)}`);
  // 网页端没有 --no-pickup，稀疏素材会自动判弱起，4 拍音在 MusicXML 里被
  // 拆成 960+960 的连音线；精确时值由 CLI 冒烟 [37] 用 --no-pickup 锁定。
  const gc = makeCaller(mod);
  gc.setMidi(pchBytes);
  const gxml = gc.call('midi2jianpu_musicxml', -1, 1) || '';
  const glong = (gxml.match(/<duration>960<\/duration>/g) || []).length;
  check(glong >= 2 && gxml.includes('<tie '),
    `网页 MusicXML 把补长的 4 拍写成分段连音，实际 ${glong} 段 960`);
}

// 新导出同样返回 char*，网页必须过 UTF8ToString（v1.26.0 在 part_info 上
// 踩过一次 TypeError，上传直接崩）。
const pageHtml2 = readFileSync(path.join(ROOT, 'docs', 'index.html'), 'utf8');
check(/UTF8ToString\(\s*wasmMod\._midi2jianpu_controller_info/.test(pageHtml2),
  'docs/index.html 的 controller_info 用 UTF8ToString 取了字符串');

// ---- 28. RPN 弯音范围：每通道、可中途改，网页三层输出必须与 CLI 一致 ----
// 旧实现把范围压成一个全局量，最后一个 RPN 覆盖全曲，网页端会把通道 1 的
// 2 半音颤音放大成 12 半音滑音。这里同时锁住导出符号、文件面板与 MusicXML 音高。
function bendRangeInfo(bytes) {
  if (!mod._midi2jianpu_bend_range_info) return '';
  const ptr = mod._malloc(bytes.length);
  mod.HEAPU8.set(bytes, ptr);
  const s = mod.UTF8ToString(mod._midi2jianpu_bend_range_info(ptr, bytes.length));
  mod._free(ptr);
  return s || '';
}
check(typeof mod._midi2jianpu_bend_range_info === 'function',
  'WASM 导出 _midi2jianpu_bend_range_info');
const brcBytes = readFixture('bendrange_channels.mid');
if (brcBytes) {
  const info = bendRangeInfo(brcBytes);
  check(/±12/.test(info) && /按通道与时间生效/.test(info),
    `WASM 报出弯音范围，实际：${info}`);
  const bc = makeCaller(mod);
  bc.setMidi(brcBytes);
  const bx = bc.call('midi2jianpu_musicxml', -1, 1) || '';
  check(/<step>C<\/step><octave>5<\/octave>/.test(bx) &&
        /<step>D<\/step><octave>4<\/octave>/.test(bx),
    '网页 MusicXML 两个通道分别升到 C5 / D4');
  const bh = bc.call('midi2jianpu_html', -1, 1) || '';
  check(bh.includes('弯音范围：±12'), '网页 HTML 页眉写明弯音范围');
  const plainB = readFixture('sample.mid');
  if (plainB) check(bendRangeInfo(plainB) === '',
    '没有 RPN 的普通文件返回空串');
}
const brtBytes = readFixture('bendrange_timeline.mid');
if (brtBytes) {
  check(/±2 \/ ±12/.test(bendRangeInfo(brtBytes)),
    'WASM 列出中途改过的两种范围');
  const bt = makeCaller(mod);
  bt.setMidi(brtBytes);
  const bx2 = bt.call('midi2jianpu_musicxml', -1, 1) || '';
  check(/<step>C<\/step><octave>5<\/octave>/.test(bx2) &&
        /<step>D<\/step><octave>4<\/octave>/.test(bx2),
    '网页 MusicXML 中途改范围不倒灌到前一个音');
}
const brxBytes = readFixture('bendrange_crosstrack.mid');
if (brxBytes) {
  check(/±12/.test(bendRangeInfo(brxBytes)),
    'WASM 认出写在另一条轨上的 RPN');
}
check(/UTF8ToString\(\s*wasmMod\._midi2jianpu_bend_range_info/.test(pageHtml2),
  'docs/index.html 的 bend_range_info 用 UTF8ToString 取了字符串');

// ---- 29. 省略状态字节：网页端也要把系统事件之后的半首曲子读回来 ----
// 旧实现把 meta / SysEx 的状态字节当成 running status，歌词之后的音符
// 会被读成 meta 事件，整段消失。这里锁住新导出、文件面板和 MusicXML。
function runningStatusInfo(bytes) {
  if (!mod._midi2jianpu_running_status_info) return '';
  const ptr = mod._malloc(bytes.length);
  mod.HEAPU8.set(bytes, ptr);
  const s = mod.UTF8ToString(mod._midi2jianpu_running_status_info(ptr, bytes.length));
  mod._free(ptr);
  return s || '';
}
check(typeof mod._midi2jianpu_running_status_info === 'function',
  'WASM 导出 _midi2jianpu_running_status_info');
for (const [name, needHint] of [
  ['running_meta.mid', true],
  ['running_sysex.mid', true],
  ['running_realtime.mid', false],
  ['running_standard.mid', false],
]) {
  const bytes = readFixture(name);
  if (!bytes) continue;
  const info = runningStatusInfo(bytes);
  if (needHint) {
    check(/补回 1 处/.test(info), `${name}：WASM 报出补回处数，实际：${info}`);
  } else {
    check(info === '', `${name}：无需补回时返回空串，实际：${info}`);
  }
  const caller = makeCaller(mod);
  caller.setMidi(bytes);
  const xml = caller.call('midi2jianpu_musicxml', -1, 1) || '';
  check(/<step>C<\/step><octave>4<\/octave>/.test(xml) &&
        /<step>D<\/step><octave>4<\/octave>/.test(xml),
    `${name}：网页 MusicXML 读出 C4 / D4 两个音`);
}
check(/UTF8ToString\(\s*wasmMod\._midi2jianpu_running_status_info/.test(pageHtml2),
  'docs/index.html 的 running_status_info 用 UTF8ToString 取了字符串');

// ---- 30. MIDI Port：网页端同样按 (port, channel) 隔离控制器 ----
// FF 21 被忽略时，port 0 的踏板会补长 port 1 的音、port 0 的 RPN ±12
// 会把 port 1 的默认 ±2 弯音一起放大。这里锁住新导出与文件面板。
function portInfo(bytes) {
  if (!mod._midi2jianpu_port_info) return '';
  const ptr = mod._malloc(bytes.length);
  mod.HEAPU8.set(bytes, ptr);
  const s = mod.UTF8ToString(mod._midi2jianpu_port_info(ptr, bytes.length));
  mod._free(ptr);
  return s || '';
}
check(typeof mod._midi2jianpu_port_info === 'function',
  'WASM 导出 _midi2jianpu_port_info');
const portPedalBytes = readFixture('port_pedal.mid');
if (portPedalBytes) {
  const info = portInfo(portPedalBytes);
  check(/MIDI Port：2 处声明，共 2 个端口/.test(info),
    `port_pedal：WASM 报出端口数，实际：${info}`);
  check(/同通道号跨端口已隔离/.test(info),
    'port_pedal：WASM 说明端口隔离结果');
}
const portBendBytes = readFixture('port_bend.mid');
if (portBendBytes) {
  const caller = makeCaller(mod);
  caller.setMidi(portBendBytes);
  const xml = caller.call('midi2jianpu_musicxml', -1, 1) || '';
  check(/<step>C<\/step><octave>5<\/octave>/.test(xml) &&
        /<step>E<\/step><octave>4<\/octave>/.test(xml),
    'port_bend：网页 MusicXML 是 C5 / E4，不是两个都按 ±12 推到 C5 / D5');
}
const portPrefixBytes = readFixture('port_prefix.mid');
if (portPrefixBytes) {
  check(/通道前缀：1 处（按规范不影响通道消息）/.test(portInfo(portPrefixBytes)),
    'port_prefix：WASM 报出 FF 20 但不重定向通道');
}
for (const name of ['port_prefix_base.mid', 'port_standard.mid', 'sample.mid']) {
  const bytes = readFixture(name);
  if (!bytes) continue;
  check(portInfo(bytes) === '', `${name}：无端口声明时返回空串`);
}
check(/UTF8ToString\(\s*wasmMod\._midi2jianpu_port_info/.test(pageHtml2),
  'docs/index.html 的 port_info 用 UTF8ToString 取了字符串');


// ---- 31. SysEx 鼓模式：网页端也要按 GS/XG 设定丢掉鼓件音 ----
// 旧实现把 0xF0 整段跳过，只按「通道 9 是鼓」硬判定，于是 GS/XG 文件里
// 被 SysEx 设成鼓的 part 会转成 `1,, 2,, #4,,` 一串低音垃圾。这里锁住新
// 导出、文件面板与网页 MusicXML。
function sysExInfo(bytes) {
  if (!mod._midi2jianpu_sysex_info) return '';
  const ptr = mod._malloc(bytes.length);
  mod.HEAPU8.set(bytes, ptr);
  const s = mod.UTF8ToString(mod._midi2jianpu_sysex_info(ptr, bytes.length));
  mod._free(ptr);
  return s || '';
}
check(typeof mod._midi2jianpu_sysex_info === 'function',
  'WASM 导出 _midi2jianpu_sysex_info');
const sxGsBytes = readFixture('sysex_gs_drum.mid');
if (sxGsBytes) {
  const info = sysExInfo(sxGsBytes);
  check(/1 处 GS\/XG 设定/.test(info) && /已跳过 8 个鼓件音/.test(info),
    `sysex_gs_drum：WASM 报出鼓模式与跳过数，实际：${info}`);
  const caller = makeCaller(mod);
  caller.setMidi(sxGsBytes);
  const xml = caller.call('midi2jianpu_musicxml', -1, 1) || '';
  const steps = [...xml.matchAll(/<step>[A-G]<\/step>/g)];
  check(steps.length === 8 && !/<octave>2<\/octave>/.test(xml),
    'sysex_gs_drum：网页 MusicXML 只有 8 个旋律音，没有低八度鼓件音');
}
const sxXgBytes = readFixture('sysex_xg_drum.mid');
if (sxXgBytes) {
  check(/已跳过 8 个鼓件音/.test(sysExInfo(sxXgBytes)),
    'sysex_xg_drum：XG 写法同样被 WASM 认出');
}
const sxResetBytes = readFixture('sysex_gs_reset.mid');
if (sxResetBytes) {
  check(/1 处 GM\/GS\/XG Reset 恢复默认/.test(sysExInfo(sxResetBytes)) &&
        /已跳过 4 个鼓件音/.test(sysExInfo(sxResetBytes)),
    'sysex_gs_reset：WASM 报出 Reset 只撤销后半段');
}
const sxCh9Bytes = readFixture('sysex_ch9_normal.mid');
if (sxCh9Bytes) {
  const caller = makeCaller(mod);
  caller.setMidi(sxCh9Bytes);
  const xml = caller.call('midi2jianpu_musicxml', -1, 1) || '';
  check([...xml.matchAll(/<step>[A-G]<\/step>/g)].length === 16,
    'sysex_ch9_normal：通道 10 改回钢琴后网页 MusicXML 有 16 个音');
}
for (const name of ['sysex_unknown.mid', 'sample.mid', 'port_standard.mid']) {
  const bytes = readFixture(name);
  if (!bytes) continue;
  check(sysExInfo(bytes) === '', `${name}：无 GS/XG 设定时返回空串`);
}
check(/UTF8ToString\(\s*wasmMod\._midi2jianpu_sysex_info/.test(pageHtml2),
  'docs/index.html 的 sysex_info 用 UTF8ToString 取了字符串');


// ---- 32. Bank Select / Program Change：网页端同样按时间轴处理 ----
// 旧实现忽略 CC0 / CC32，XG 用 `Bn 00 7F` 指定的鼓通道会转成假旋律；
// 0xC0 也只记最后一个音色号，中途换乐器时前半首声部被错标。
function bankInfo(bytes) {
  if (!mod._midi2jianpu_bank_info) return '';
  const ptr = mod._malloc(bytes.length);
  mod.HEAPU8.set(bytes, ptr);
  const s = mod.UTF8ToString(mod._midi2jianpu_bank_info(ptr, bytes.length));
  mod._free(ptr);
  return s || '';
}
check(typeof mod._midi2jianpu_bank_info === 'function',
  'WASM 导出 _midi2jianpu_bank_info');
const bxBytes = readFixture('bank_xg_drum.mid');
if (bxBytes) {
  const info = bankInfo(bxBytes);
  check(/2 处 Bank Select/.test(info) && /1 条 XG 鼓组通道/.test(info),
    `bank_xg_drum：WASM 报出 Bank Select 与鼓组通道，实际：${info}`);
  const caller = makeCaller(mod);
  caller.setMidi(bxBytes);
  const xml = caller.call('midi2jianpu_musicxml', -1, 1) || '';
  const steps = [...xml.matchAll(/<step>[A-G]<\/step>/g)];
  check(steps.length === 8 && !/<octave>2<\/octave>/.test(xml),
    'bank_xg_drum：网页 MusicXML 只有 8 个旋律音，没有低八度鼓件音');
}
const bnBytes = readFixture('bank_normal.mid');
if (bnBytes) {
  const caller = makeCaller(mod);
  caller.setMidi(bnBytes);
  const xml = caller.call('midi2jianpu_musicxml', -1, 1) || '';
  check([...xml.matchAll(/<step>[A-G]<\/step>/g)].length >= 16,
    'bank_normal：普通音色库对照的网页 MusicXML 不丢音');
}
const ptBytes = readFixture('program_timeline.mid');
if (ptBytes) {
  const caller = makeCaller(mod);
  caller.setMidi(ptBytes);
  const xml = caller.call('midi2jianpu_musicxml', -1, 1) || '';
  check(/<instrument-name>钢琴<\/instrument-name>/.test(xml) &&
        !/<instrument-name>小提琴<\/instrument-name>/.test(xml),
    'program_timeline：网页 MusicXML 的声部乐器名按首音时刻取钢琴');
}
for (const name of ['program_timeline.mid', 'sample.mid', 'sysex_gs_drum.mid']) {
  const bytes = readFixture(name);
  if (!bytes) continue;
  check(bankInfo(bytes) === '', `${name}：无 Bank Select 时返回空串`);
}
check(/UTF8ToString\(\s*wasmMod\._midi2jianpu_bank_info/.test(pageHtml2),
  'docs/index.html 的 bank_info 用 UTF8ToString 取了字符串');


// ---- 33. --track 按轨隔离：网页端段落选择 ----
// CLI 的 --track 过去只在末尾丢音符，网页则根本没法选段落。现在两者共用
// 解析层的 trackFilter，网页通过 _midi2jianpu_set_track 设置。
check(typeof mod._midi2jianpu_set_track === 'function',
  'WASM 导出 _midi2jianpu_set_track');
check(typeof mod._midi2jianpu_track_info === 'function',
  'WASM 导出 _midi2jianpu_track_info');

function withBytes(name, fn) {
  const bytes = readFixture(name);
  if (!bytes) return;
  const ptr = mod._malloc(bytes.length);
  mod.HEAPU8.set(bytes, ptr);
  try { fn(ptr, bytes.length); } finally { mod._free(ptr); }
}
function trackInfo(bytes) {
  const ptr = mod._malloc(bytes.length);
  mod.HEAPU8.set(bytes, ptr);
  const s = mod.UTF8ToString(mod._midi2jianpu_track_info(ptr, bytes.length));
  mod._free(ptr);
  return s || '';
}
const tlWasm = readFixture('track_leak.mid');
if (tlWasm) {
  mod._midi2jianpu_set_track(-1);
  check(trackInfo(tlWasm) === '', '默认不指定轨时 track_info 返回空串');
  withBytes('track_leak.mid', (p, n) => {
    check(mod._midi2jianpu_midi_lyric_count(p, n) === 16,
      '默认读取两轨共 16 个歌词音节');
  });

  mod._midi2jianpu_set_track(0);
  const ti0 = trackInfo(tlWasm);
  check(/按轨隔离/.test(ti0) && /轨 0/.test(ti0),
    `选中轨 0 后 track_info 说明隔离范围，实际：${ti0}`);
  withBytes('track_leak.mid', (p, n) => {
    check(mod._midi2jianpu_midi_lyric_count(p, n) === 0,
      '选中轨 0 后网页读到的内置歌词音节数为 0');
  });
  const callerT0 = makeCaller(mod);
  callerT0.setMidi(tlWasm);
  const txt0 = callerT0.call('midi2jianpu_parse', -1, 1) || '';
  check(!/歌词|力度/.test(txt0) || !/p\b/.test(txt0),
    '选中轨 0 后网页简谱不混入别轨的力度记号');

  mod._midi2jianpu_set_track(1);
  withBytes('track_leak.mid', (p, n) => {
    check(mod._midi2jianpu_midi_lyric_count(p, n) === 16,
      '切到轨 1 后网页又读回该轨的 16 个歌词音节');
  });
  check(/轨 1/.test(trackInfo(tlWasm)), 'track_info 跟着段落切换改口');

  // 全局状态：测试完必须复位，否则后面的断言会沿用上一段选择
  mod._midi2jianpu_set_track(-1);
  check(trackInfo(tlWasm) === '', '复位到 -1 后 track_info 重新为空');
}
const tcWasm = readFixture('track_conductor.mid');
if (tcWasm) {
  mod._midi2jianpu_set_track(1);
  withBytes('track_conductor.mid', (p, n) => {
    const fh = mod.UTF8ToString(mod._midi2jianpu_file_header_info(p, n));
    check(typeof fh === 'string', '选段后文件头提示仍能正常返回');
  });
  const callerTc = makeCaller(mod);
  callerTc.setMidi(tcWasm);
  const xmlTc = callerTc.call('midi2jianpu_musicxml', -1, 1) || '';
  check(/<beats>3<\/beats>/.test(xmlTc),
    '选中轨 1 后网页 MusicXML 仍用别轨 conductor 的 3/4 拍');
  mod._midi2jianpu_set_track(-1);
}
check(/UTF8ToString\(\s*wasmMod\._midi2jianpu_track_info/.test(pageHtml2),
  'docs/index.html 的 track_info 用 UTF8ToString 取了字符串');
check(/_midi2jianpu_set_track\(currentTrack\(\)\)/.test(pageHtml2),
  'docs/index.html 转换前按当前下拉框重设段落');
check(/<select id="track"/.test(pageHtml2) && /function fillTrackOptions/.test(pageHtml2),
  'docs/index.html 提供段落下拉框并按 MThd 轨数填充');

// ---- 34. WASM 产物版本必须与源码一致（防止只发 CLI、忘记重编 wasm） ----
// 历史教训：v1.35.0 ~ v1.37.0 的 CLI 都修好了，docs/wasm/ 却一直是 1.34.0 的
// 产物，网页转换器连续三版拿不到修复、页脚版本号也一直滞后。这里直接比对
// version.hpp 与 WASM 生成的页脚，漏重编就当场失败。
const versionHpp = readFileSync(path.join(ROOT, 'include', 'midi2jianpu', 'version.hpp'), 'utf8');
const srcVer = (versionHpp.match(/M2J_VERSION\s+"([^"]+)"/) || [])[1] || '';
const wasmVer = ((htmlLyric || '').match(/<footer[^>]*>[^<]*v([\d.]+)/) || [])[1] || '';
check(/^\d+\.\d+\.\d+$/.test(srcVer), `version.hpp 能读出版本号（${srcVer}）`);
check(srcVer !== '' && srcVer === wasmVer,
  `WASM 产物与源码版本一致（v${srcVer}）`,
  `页脚 v${wasmVer} / 源码 v${srcVer}；改了 src/ 后请重编 docs/wasm/`);

// ---- 35. v1.36.0 CC64 踏板迟滞：网页端也要延到真正的释放点 ----
// 127 -> 50（死区 33..63）-> 0：旧二值判定在 50 处就断开（1 拍），迟滞后
// 延到真正释放点（2 拍）。CLI 有素材和冒烟组，网页端由这里兜住。
const phPath = path.join(FIXTURES, 'pedal_hysteresis.mid');
if (existsSync(phPath)) {
  const ph = makeCaller(mod);
  ph.setMidi(new Uint8Array(readFileSync(phPath)));
  ph.setLyrics('');
  const phXml = ph.call('midi2jianpu_musicxml', -1, 1) || '';
  const pitched = [...phXml.matchAll(/<note\b[\s\S]*?<\/note>/g)]
    .map((m) => m[0]).filter((b) => !/<rest\b/.test(b));
  check(pitched.length === 1, '踏板迟滞素材在网页端解析出 1 个实音', phXml.slice(0, 200));
  check(pitched.length === 1 && /<duration>960<\/duration>/.test(pitched[0])
    && /<type>half<\/type>/.test(pitched[0]),
    'CC64 迟滞：网页 WASM 延到真正释放点（960 tick / half，旧产物是 1 拍）',
    pitched[0] || '');
}

// ---- 36. 截断 MIDI：网页端同样不能越过 buffer 末尾读幻影音符 ----
// 浏览器里最容易遇到的就是「下载到一半的 .mid」。旧 WASM 会继续读线性内存
// 里相邻堆块，2 音素材冒出 10 个音；修复后停在文件末尾并报告数据不完整。
const truncPath = path.join(FIXTURES, 'truncated.mid');
if (existsSync(truncPath)) {
  const tmCaller = makeCaller(mod);
  tmCaller.setMidi(new Uint8Array(readFileSync(truncPath)));
  tmCaller.setLyrics('');
  const tmXml = tmCaller.call('midi2jianpu_musicxml', -1, 1) || '';
  const tmNotes = [...tmXml.matchAll(/<note\b[\s\S]*?<\/note>/g)]
    .map((m) => m[0]).filter((b) => !/<rest\b/.test(b));
  check(tmNotes.length === 2,
    '截断 MIDI 在网页 WASM 里也只出 2 个实音（旧产物会冒出幻影音符）',
    tmXml.slice(0, 200));
  const tmRaw = new Uint8Array(readFileSync(truncPath));
  const tmPtr = mod._malloc(tmRaw.length);
  mod.HEAPU8.set(tmRaw, tmPtr);
  const tmLyric = mod._midi2jianpu_midi_lyric_count(tmPtr, tmRaw.length);
  mod._free(tmPtr);
  check(tmLyric === 0, '截断 MIDI 在网页端不冒出乱码歌词音节');
  withBytes('truncated.mid', (p, n) => {
    const fh = mod.UTF8ToString(mod._midi2jianpu_file_header_info(p, n));
    check(/文件数据不完整/.test(fh), 'file_header_info 向网页报告数据被截断', fh);
  });
}


// ---- 37. v1.39.0 CC66 持音踏板：网页端也要保持踩下瞬间正按着的键 ----
// 3 个和弦音应延到 1920 tick，踩下之后新弹的 D4 仍是 480 tick。旧 WASM 会把
// CC66 当普通控制器跳过，四个音全是 480；controller_info 也要向网页说明。
const sostPath = path.join(FIXTURES, 'sostenuto.mid');
if (existsSync(sostPath)) {
  const sostCaller = makeCaller(mod);
  sostCaller.setMidi(new Uint8Array(readFileSync(sostPath)));
  sostCaller.setLyrics('');
  const sostXml = sostCaller.call('midi2jianpu_musicxml', -1, 1) || '';
  const sostNotes = [...sostXml.matchAll(/<note\b[\s\S]*?<\/note>/g)]
    .map((m) => m[0]).filter((b) => !/<rest\b/.test(b));
  const sostDurs = sostNotes
    .map((b) => (b.match(/<duration>(\d+)<\/duration>/) || [])[1] || '?');
  check(sostDurs.length === 4 &&
    sostDurs.filter((d) => d === '1920').length === 3 &&
    sostDurs.filter((d) => d === '480').length === 1,
    'CC66 在网页 WASM 里保持 3 个和弦音（1920），新弹的音仍是 480',
    sostDurs.join(','));
  withBytes('sostenuto.mid', (p, n) => {
    const ci = mod.UTF8ToString(mod._midi2jianpu_controller_info(p, n));
    check(/持音踏板/.test(ci), 'controller_info 向网页报告持音踏板', ci);
  });
}



// ---- 38. v1.40.0 CC11 渐强渐弱按通道归属 ----
// 旋律(ch0)渐弱、伴奏(ch1)渐强：旧 WASM 会把两条 wedge 合并，网页谱面印
// cresc. 而力度写 ff→pp。v1.43.0 起两个声部各自印自己的渐变，v1.45.0
// 起同轨单 part 的 MusicXML 也按 <voice> 各写一份 dim. / cresc.。
const dcPath = path.join(FIXTURES, 'dyn_channels.mid');
if (existsSync(dcPath)) {
  const dc = makeCaller(mod);
  dc.setMidi(new Uint8Array(readFileSync(dcPath)));
  dc.setLyrics('');
  const dcXml = dc.call('midi2jianpu_musicxml', -1, 1) || '';
  check(/diminuendo/.test(dcXml) && /crescendo/.test(dcXml),
    '网页 WASM 的 MusicXML 两个声部各写自己的渐弱 / 渐强', dcXml.slice(0, 200));
  check(/<voice>1<\/voice>/.test(dcXml) && /<voice>2<\/voice>/.test(dcXml),
    '网页 WASM 的 direction 带 <voice> 归属', dcXml.slice(0, 200));
  const dcHtml = dc.call('midi2jianpu_html', -1, 1) || '';
  check(/>dim\.</.test(dcHtml) && />cresc\.</.test(dcHtml),
    '网页 WASM 的 HTML 两个声部各写自己的渐变（v1.43.0）', dcHtml.slice(0, 200));
  check(/Verse/.test(dcHtml), '网页 WASM 仍显示全局段落名');
}


// ---- 39. v1.41.0 CC67 软踏板：网页端也要衰减起音力度 ----
// 旧 WASM 把 CC67 当普通控制器跳过，播放 JSON 里全是原始 velocity，
// 两处重音都还在；controller_info 也不会提「软踏板」。
const spPath = path.join(FIXTURES, 'soft_pedal.mid');
if (existsSync(spPath)) {
  const sp = makeCaller(mod);
  sp.setMidi(new Uint8Array(readFileSync(spPath)));
  sp.setLyrics('');
  const spNotes = JSON.parse(sp.call('midi2jianpu_get_notes_json', -1, 1) || '[]');
  const spVel = spNotes.map((n) => n.velocity);
  check(spVel.includes(77) && spVel.includes(42) && spVel.includes(110),
    'WASM 播放 JSON 里踏板内 110→77、60→42，踏板外仍是 110', spVel.join(','));
  const spXml = sp.call('midi2jianpu_musicxml', -1, 1) || '';
  check((spXml.match(/<accent\/>/g) || []).length === 1,
    '网页 WASM 的 MusicXML 只剩 1 个重音记号');
  withBytes('soft_pedal.mid', (p, n) => {
    const ci = mod.UTF8ToString(mod._midi2jianpu_controller_info(p, n));
    check(/软踏板/.test(ci), 'controller_info 向网页报告软踏板', ci);
  });
}
// ---- 39b. v1.46.0 软踏板系数可配：网页端也能调强度 / 关闭 ----
// 默认 0.7 把踏板内 110→77、60→42；把系数调到 1 或关闭，网页 JSON 应还原成
// 原始 110 / 60，且 controller_info 不再提软踏板。改完必须把全局系数复位成 0.7，
// 否则后面的测试（含其它曲子的 CC67）会沿用 1.0。
if (existsSync(spPath)) {
  mod._midi2jianpu_set_soft_pedal_scale(1.0);
  const sp1 = makeCaller(mod);
  sp1.setMidi(new Uint8Array(readFileSync(spPath)));
  sp1.setLyrics('');
  const sp1Notes = JSON.parse(sp1.call('midi2jianpu_get_notes_json', -1, 1) || '[]');
  const sp1Vel = sp1Notes.map((n) => n.velocity);
  check(!sp1Vel.includes(77) && !sp1Vel.includes(42) && sp1Vel.includes(110),
    'scale=1.0 网页 JSON 还原原始力度(无衰减)', sp1Vel.join(','));
  withBytes('soft_pedal.mid', (p, n) => {
    const ci = mod.UTF8ToString(mod._midi2jianpu_controller_info(p, n));
    check(!/软踏板/.test(ci), 'scale=1.0 时 controller_info 不报软踏板', ci);
  });

  mod._midi2jianpu_set_soft_pedal_scale(0.0);
  const sp0 = makeCaller(mod);
  sp0.setMidi(new Uint8Array(readFileSync(spPath)));
  sp0.setLyrics('');
  const sp0Notes = JSON.parse(sp0.call('midi2jianpu_get_notes_json', -1, 1) || '[]');
  const sp0Vel = sp0Notes.map((n) => n.velocity);
  check(sp0Vel.includes(110) && !sp0Vel.includes(77),
    'scale=0.0 关闭软踏板，网页 JSON 也不衰减', sp0Vel.join(','));

  mod._midi2jianpu_set_soft_pedal_scale(0.5);
  const sp5 = makeCaller(mod);
  sp5.setMidi(new Uint8Array(readFileSync(spPath)));
  sp5.setLyrics('');
  const sp5Notes = JSON.parse(sp5.call('midi2jianpu_get_notes_json', -1, 1) || '[]');
  const sp5Vel = sp5Notes.map((n) => n.velocity);
  check(sp5Vel.includes(55) && sp5Vel.includes(30) && sp5Vel.includes(110),
    'scale=0.5 网页 JSON 半衰减(110→55、60→30)', sp5Vel.join(','));

  // 复位成默认 0.7，避免污染后续测试
  mod._midi2jianpu_set_soft_pedal_scale(0.7);
}
// ---- 40. v1.42.0 SMF chunk 级边界重同步 ----
// 旧 WASM 遇到 MThd 后的私有 chunk / 写错的首轨长度会直接判「无音符」，
// 网页端只剩一个空谱面。修复后应照常读出 4 个音，并把结构提示送进页眉。
const crPath = path.join(FIXTURES, 'chunk_resync.mid');
if (existsSync(crPath)) {
  mod._midi2jianpu_set_track(-1);
  const cr = makeCaller(mod);
  cr.setMidi(new Uint8Array(readFileSync(crPath)));
  cr.setLyrics('');
  const crXml = cr.call('midi2jianpu_musicxml', -1, 1) || '';
  const crNotes = [...crXml.matchAll(/<note\b[\s\S]*?<\/note>/g)]
    .map((m) => m[0]).filter((b) => !/<rest\b/.test(b));
  check(crNotes.length === 4,
    'WASM 重同步后仍读出 4 个旋律音（旧产物是 0 个）', String(crNotes.length));
  const crHtml = cr.call('midi2jianpu_html', -1, 1) || '';
  check(/文件结构提示/.test(crHtml) && /非 MTrk 扩展块/.test(crHtml) &&
    /重同步 1 条轨道边界/.test(crHtml),
    'WASM HTML 页眉同时写明扩展块与边界重同步', crHtml.slice(0, 220));
  withBytes('chunk_resync.mid', (p, n) => {
    const fh = mod.UTF8ToString(mod._midi2jianpu_file_header_info(p, n));
    check(/文件结构提示/.test(fh) && /重同步/.test(fh),
      'file_header_info 向网页报告 chunk 边界修复', fh);
  });
  withBytes('expression.mid', (p, n) => {
    const fh = mod.UTF8ToString(mod._midi2jianpu_file_header_info(p, n));
    check(!/文件结构提示/.test(fh), '合法 SMF 在网页端不误报结构异常', fh);
  });
}

// ---- 41. v1.43.0 通道级楔形：网页端每个声部各写自己的渐强渐弱 ----
// 旧 WASM 把 wedge 和全局段落名一起塞进「只写第一个声部 / 第一个 part」
// 的门控，第二条轨的 crescendo 连同 wedge stop 一起消失。修复后 MusicXML
// 两个 part 各写一份 wedge，HTML 两行谱各写一个，全局 Verse 仍只出现一次。
const wtPath = path.join(FIXTURES, 'wedge_tracks.mid');
if (existsSync(wtPath)) {
  mod._midi2jianpu_set_track(-1);
  const wt = makeCaller(mod);
  wt.setMidi(new Uint8Array(readFileSync(wtPath)));
  wt.setLyrics('');
  const wtXml = wt.call('midi2jianpu_musicxml', -1, 1) || '';
  const p1 = wtXml.indexOf('<part id="P1"');
  const p2 = wtXml.indexOf('<part id="P2"');
  check(p1 >= 0 && p2 > p1, 'WASM MusicXML 写出两个 part');
  const part1 = wtXml.slice(p1, p2);
  const part2 = wtXml.slice(p2);
  check(/diminuendo/.test(part1) && !/crescendo/.test(part1),
    'WASM P1 只写旋律通道的渐弱');
  check(/crescendo/.test(part2) && !/diminuendo/.test(part2),
    'WASM P2 补写伴奏通道的渐强');
  check((wtXml.match(/<words>Verse<\/words>/g) || []).length === 1,
    'WASM 全局段落名只写一次');
  const wtHtml = wt.call('midi2jianpu_html', -1, 1) || '';
  check(/>dim\.</.test(wtHtml) && />cresc\.</.test(wtHtml),
    'WASM HTML 两行谱各写自己的渐变');
  check((wtHtml.match(/class='section'/g) || []).length === 1,
    'WASM HTML 全局段落名只印一次');
}

// ---- 42. v1.44.0 越界长度字段：网页端按锚点重同步，不误报数据截断 ----
// 旧 WASM 把「声明长度越过文件末尾」一律当成真·截断，两轨并成一轨、
// 段落 #1 选不到旋律，文件面板还误报数据不完整。修复后应读出 4 个音，
// file_header_info / HTML 页眉只报「重同步 1 条轨道边界」。
const olPath = path.join(FIXTURES, 'oversized_len.mid');
if (existsSync(olPath)) {
  withBytes('oversized_len.mid', (p, n) => {
    const fh = mod.UTF8ToString(mod._midi2jianpu_file_header_info(p, n));
    check(/文件结构提示/.test(fh) && /重同步 1 条轨道边界/.test(fh),
      'file_header_info 报告越界长度字段的边界修复', fh);
    check(!/文件数据不完整/.test(fh), '网页端不误报文件数据不完整', fh);
  });
  mod._midi2jianpu_set_track(-1);
  const ol = makeCaller(mod);
  ol.setMidi(new Uint8Array(readFileSync(olPath)));
  ol.setLyrics('');
  const olNotes = JSON.parse(ol.call('midi2jianpu_get_notes_json', -1, 1) || 'null');
  check(Array.isArray(olNotes) && olNotes.length === 4,
    'WASM 越界长度字段后仍读出 4 个旋律音', String(olNotes && olNotes.length));
  const olHtml = ol.call('midi2jianpu_html', -1, 1) || '';
  check(/文件结构提示/.test(olHtml) && !/文件数据不完整/.test(olHtml),
    'WASM HTML 页眉只报结构修复', olHtml.slice(0, 220));

  // 网页段落选择：#1 才是真正的旋律轨，旧产物并轨后这里会是空谱面
  mod._midi2jianpu_set_track(1);
  const ol2 = makeCaller(mod);
  ol2.setMidi(new Uint8Array(readFileSync(olPath)));
  ol2.setLyrics('');
  const ol2Notes = JSON.parse(ol2.call('midi2jianpu_get_notes_json', -1, 1) || 'null');
  check(Array.isArray(ol2Notes) && ol2Notes.length === 4,
    'WASM 段落选择 #1 能选中真正的旋律轨', String(ol2Notes && ol2Notes.length));
  mod._midi2jianpu_set_track(-1);

  withBytes('truncated.mid', (p, n) => {
    const fh = mod.UTF8ToString(mod._midi2jianpu_file_header_info(p, n));
    check(/文件数据不完整/.test(fh) && !/文件结构提示/.test(fh),
      '真·截断文件在网页端仍按 v1.38 报数据不完整', fh);
  });
}

// ---- 43. v1.45.0 同轨多声部：WASM MusicXML 按 <voice> 各写 direction ----
// 旧 WASM 整个 part 只取一个主导通道，第二 / 第三声部的 crescendo 与
// p→ff 力度档位整条丢失。修复后每个 <voice> 各写一份、wedge number
// 递增到 3，direction 带 <voice> 归属，全局段落名仍只写一次。
const vtPath = path.join(FIXTURES, 'voice_trio.mid');
if (existsSync(vtPath)) {
  mod._midi2jianpu_set_track(-1);
  const vt = makeCaller(mod);
  vt.setMidi(new Uint8Array(readFileSync(vtPath)));
  vt.setLyrics('');
  const vtXml = vt.call('midi2jianpu_musicxml', -1, 1) || '';
  check((vtXml.match(/crescendo/g) || []).length === 2 &&
        (vtXml.match(/diminuendo/g) || []).length === 1,
    'WASM 三个声部写出 2 个渐强 + 1 个渐弱');
  check(/number="1"/.test(vtXml) && /number="2"/.test(vtXml) &&
        /number="3"/.test(vtXml),
    'WASM wedge number 按声部递增到 3');
  check(/<direction[^>]*>[\s\S]*?<voice>3<\/voice>[\s\S]*?<\/direction>/.test(vtXml),
    'WASM direction 带 <voice>3 归属');
  check((vtXml.match(/<words>Verse<\/words>/g) || []).length === 1,
    'WASM 全局段落名仍只写一次');

  const dcW = makeCaller(mod);
  dcW.setMidi(new Uint8Array(readFileSync(path.join(FIXTURES, 'dyn_channels.mid'))));
  dcW.setLyrics('');
  const dcXml = dcW.call('midi2jianpu_musicxml', -1, 1) || '';
  check(/crescendo/.test(dcXml) && /<dynamics><p\/>/.test(dcXml),
    'WASM dyn_channels 补回副声部 crescendo 与 p 力度');

  const wsW = makeCaller(mod);
  wsW.setMidi(new Uint8Array(readFileSync(path.join(FIXTURES, 'wedge_tracks.mid'))));
  wsW.setLyrics('');
  const wsXml = wsW.call('midi2jianpu_musicxml', -1, 1) || '';
  check(!wsXml.includes('        <voice>'),
    'WASM 单声部 direction 不写多余的 <voice>');
}

// ---- 44. v1.47.0 键盘大谱表：WASM MusicXML 也排成上下两行谱 ----
// 旧产物整份 part 只挑一个谱号，钢琴左手被塞进高音谱号、满屏加线。修复后
// 声明 <staves>2</staves>、上下谱表各一个谱号，音符带 <staff> 归属。
const gsPath = path.join(FIXTURES, 'grand_staff.mid');
if (existsSync(gsPath)) {
  mod._midi2jianpu_set_track(-1);
  const gs = makeCaller(mod);
  gs.setMidi(new Uint8Array(readFileSync(gsPath)));
  gs.setLyrics('');
  const gsXml = gs.call('midi2jianpu_musicxml', -1, 1) || '';
  check(gsXml.includes('<staves>2</staves>'), 'WASM 大谱表声明两行谱');
  check(gsXml.includes('<clef number="1"><sign>G</sign><line>2</line></clef>') &&
        gsXml.includes('<clef number="2"><sign>F</sign><line>4</line></clef>'),
    'WASM 上下谱表分别是高音 / 低音谱号');
  check((gsXml.match(/<staff>1<\/staff>/g) || []).length >= 8 &&
        (gsXml.match(/<staff>2<\/staff>/g) || []).length >= 5,
    'WASM 音符按音高分流到上下谱表');

  const plainWs = makeCaller(mod);
  plainWs.setMidi(new Uint8Array(readFileSync(path.join(FIXTURES, 'sample.mid'))));
  plainWs.setLyrics('');
  const plainXml = plainWs.call('midi2jianpu_musicxml', -1, 1) || '';
  check(!plainXml.includes('<staves>') && !plainXml.includes('<staff>'),
    'WASM 普通旋律声部仍保持单谱表输出');
}

// ---- 45. v1.48.0 小节中途变速：速度记号保留小节内位置 ----
// 旧产物按小节把速度记号收进 map<bar, mark>，同一小节多个阶跃只留最后一个，
// 且一律写在小节开头 —— 网页导出的 MusicXML 会被 MuseScore 整小节按新速度播。
const mtPath = path.join(FIXTURES, 'midtempo.mid');
if (existsSync(mtPath)) {
  mod._midi2jianpu_set_track(-1);
  const mt = makeCaller(mod);
  mt.setMidi(new Uint8Array(readFileSync(mtPath)));
  mt.setLyrics('');
  const mtXml = mt.call('midi2jianpu_musicxml', -1, 1) || '';
  check(mtXml.includes('<per-minute>90</per-minute>') &&
        mtXml.includes('<per-minute>60</per-minute>'),
    'WASM 同一小节的 90 / 60 两个阶跃都写出来');
  check(mtXml.includes('<offset>480</offset>') &&
        mtXml.includes('<offset>1440</offset>'),
    'WASM 中途变速用 <offset> 标回小节内拍位');
  check((mtXml.match(/<offset>/g) || []).length === 2,
    'WASM 只给中途变速的点写 <offset>');

  const rp = makeCaller(mod);
  rp.setMidi(new Uint8Array(readFileSync(path.join(FIXTURES, 'tempo_ramp.mid'))));
  rp.setLyrics('');
  const rpXml = rp.call('midi2jianpu_musicxml', -1, 1) || '';
  check((rpXml.match(/<offset>960<\/offset>/g) || []).length === 2,
    'WASM 渐快 / 渐慢落定点回到该小节第 3 拍');
}

// ---- 46. v1.49.0 小节中途换力度档：档位写到真实拍位 ----
// 旧产物只在每小节开头取 dynamicAt(barStart)，中途的 p 被 f 顶掉、整条丢失，
// f 也被挤到下一小节 —— 网页导出的谱面力度与 MIDI 播放对不上。
const dsPath = path.join(FIXTURES, 'dyn_step.mid');
if (existsSync(dsPath)) {
  mod._midi2jianpu_set_track(-1);
  const ds = makeCaller(mod);
  ds.setMidi(new Uint8Array(readFileSync(dsPath)));
  ds.setLyrics('');
  const dsXml = ds.call('midi2jianpu_musicxml', -1, 1) || '';
  check(dsXml.includes('<dynamics><ff/></dynamics>') &&
        dsXml.includes('<dynamics><p/></dynamics>') &&
        dsXml.includes('<dynamics><f/></dynamics>') &&
        dsXml.includes('<dynamics><mp/></dynamics>'),
    'WASM 四个力度档位都写出来，中途换档不再整条丢失');
  check(dsXml.includes('<offset>480</offset>') &&
        dsXml.includes('<offset>1440</offset>'),
    'WASM 中途换档用 <offset> 标回小节内拍位');
  check((dsXml.match(/<offset>/g) || []).length === 2,
    'WASM 只给小节中途的换档写 <offset>');

  const vt = makeCaller(mod);
  vt.setMidi(new Uint8Array(readFileSync(path.join(FIXTURES, 'voice_trio.mid'))));
  vt.setLyrics('');
  const vtXml = vt.call('midi2jianpu_musicxml', -1, 1) || '';
  check(!vtXml.includes('<offset>'),
    'WASM 渐变段内部的中间档位不逐个补 <offset>');
}

// ---- 47. v1.50.0 文本谱 / HTML 的中途换档落到真实拍位 ----
// 旧产物只在 openBar 里取一次 dynamicAt，网页谱面同样把第 2 拍的 p 整条丢掉、
// 把 f 顶到下一小节。现在浮标挂在出事那个音头上，样式只在用到时才随页输出
// （没有这种记号的谱子必须保持原样字节）。
if (existsSync(dsPath)) {
  const dsh = makeCaller(mod);
  dsh.setMidi(new Uint8Array(readFileSync(dsPath)));
  dsh.setLyrics('');
  const dsHtml = dsh.call('midi2jianpu_html', -1, 1) || '';
  check((dsHtml.match(/class='dynx'>p</g) || []).length === 1,
    'WASM HTML 把中途换档的 p 浮在第 2 拍那个音头上');
  check((dsHtml.match(/class='dynx'>f</g) || []).length === 1,
    'WASM HTML 的 f 落在第 4 拍，不再顶到下一小节');
  check(dsHtml.includes('.dynx{'),
    'WASM HTML 在用到浮标时随页带 dynx 样式');

  const plainCaller = makeCaller(mod);
  plainCaller.setMidi(new Uint8Array(readFileSync(path.join(FIXTURES, 'chords.mid'))));
  plainCaller.setLyrics('');
  const plainHtml = plainCaller.call('midi2jianpu_html', -1, 1) || '';
  check(!plainHtml.includes('dynx'),
    'WASM 没有中途换档的谱子不输出 dynx 样式（既有输出逐字节不变）');
}

// ---- 48. v1.51.0 调号链路（网页端没有移调参数，锁住共享 core 不被带偏）----
// --transpose 改成「音符 + MIDI 调号一起平移」后，调号解析/中途转调这条
// 共用链路必须一字不动：网页端读同一份 core，1=Bb → 1=D 的唱名与
// MusicXML 的 <fifths> -2 / 2 都要保持原样。
const tkPath = path.join(FIXTURES, 'transpose_key.mid');
if (existsSync(tkPath)) {
  const tk = makeCaller(mod);
  tk.setMidi(new Uint8Array(readFileSync(tkPath)));
  tk.setLyrics('');
  const tkText = tk.call('midi2jianpu_parse', -1, 1) || '';
  check(tkText.includes('1=Bb') && tkText.includes('1=D'),
    'WASM 文本谱仍读作 1=Bb → 1=D（移调改动没有波及不传移调的路径）', tkText);
  check(!tkText.includes('2 3 4 5'),
    'WASM 不带移调时唱名仍是 1 2 3 4');

  const tkXml = tk.call('midi2jianpu_musicxml', -1, 1) || '';
  check(tkXml.includes('<fifths>-2</fifths>') &&
        tkXml.includes('<fifths>2</fifths>'),
    'WASM MusicXML 的调号仍是 -2（Bb）与 2（D）');

  const tkHtml = tk.call('midi2jianpu_html', -1, 1) || '';
  check(tkHtml.includes('1=Bb'),
    'WASM HTML 页眉仍写 1=Bb');
}

// ---- 49. v1.52.0 文本谱和弦并格（三端里只有文本谱是「横排」的）----
// sample.mid 钢琴左手是 C-E-G 三个全音符：旧版一个音印一格，4/4 的小节被
// 写成 12 拍。网页端读同一份 core，文本谱必须同步并成 `1,+3,+5,`，
// 而 HTML（竖排和弦列）与 MusicXML（<chord/>）本来就没错，不能被带偏。
const smPath = path.join(FIXTURES, 'sample.mid');
if (existsSync(smPath)) {
  const sm = makeCaller(mod);
  sm.setMidi(new Uint8Array(readFileSync(smPath)));
  sm.setLyrics('');
  const smText = sm.call('midi2jianpu_parse', -1, 1) || '';
  check(smText.includes('1,+3,+5, - - -'),
    'WASM 文本谱把同拍三音并成一格 `1,+3,+5, - - -`', smText);
  check(!smText.includes('3, - - -'),
    'WASM 文本谱里和弦音不再各占一格');

  const smHtml = sm.call('midi2jianpu_html', -1, 1) || '';
  check(smHtml.includes("class='note chordcol whole'"),
    'WASM HTML 仍用竖排和弦列（chordcol），没被文本谱改动波及');
}

// ---- 50. v1.53.0 时值折算不许把小节撑过拍号（网页端同一份 core）----
// overfull.mid 第 2 小节是「16 分音符 + 3.75 拍休止」，第 3 小节是
// 「四分音符 + 1 拍休止 + 倚音 + 二分音符」。文本谱必须拆成 `0 - -` + `-_.`，
// 倚音也不能再把前面的休止吞掉；HTML 走视觉排版，本来就不受时值字符串影响。
const ofPath = path.join(FIXTURES, 'overfull.mid');
if (existsSync(ofPath)) {
  const of = makeCaller(mod);
  of.setMidi(new Uint8Array(readFileSync(ofPath)));
  of.setLyrics('');
  const ofText = of.call('midi2jianpu_parse', -1, 1) || '';
  check(ofText.includes('0 - - -_.'),
    'WASM 文本谱把 3.75 拍休止拆成 `0 - -` + `-_.`', ofText);
  check(!ofText.includes('0 - - -  |'),
    'WASM 文本谱里休止不再把小节撑过小节线');
  check(ofText.includes("0 (2')5'"),
    'WASM 文本谱保留倚音前面的休止', ofText);

  const ofHtml = of.call('midi2jianpu_html', -1, 1) || '';
  check(ofHtml.includes("class='note sixteenth'") &&
        ofHtml.includes("class='note half'"),
    'WASM HTML 的音符时值排版不受影响');
}

// ---- 51. v1.54.0 门限空档并进前一个音（网页端同一份 core）----
// gate_gap.mid 的四分音符只响 400 tick（DAW 门限 85%），空档 80 tick 在
// 谱面上写不出休止符。修复前文本谱一小节只剩 3 拍、MusicXML 却补出 16 分
// 休止，三端对不上；网页端读同一份 core，必须一起变成四个四分音符。
const ggPath = path.join(FIXTURES, 'gate_gap.mid');
if (existsSync(ggPath)) {
  const gg = makeCaller(mod);
  gg.setMidi(new Uint8Array(readFileSync(ggPath)));
  gg.setLyrics('');
  const ggText = gg.call('midi2jianpu_parse', -1, 1) || '';
  check(ggText.includes('1 2 3 4  |'),
    'WASM 文本谱第 1 小节读作四个四分音符（旧版只有 3 拍）', ggText);
  check(ggText.includes('5 0 6 7  |'),
    'WASM 文本谱保留真四分休止');
  check(!ggText.includes('1_. 2_. 3_. 4_.'),
    'WASM 文本谱不再写成「附点八分」串');

  const ggXml = gg.call('midi2jianpu_musicxml', -1, 1) || '';
  check(!ggXml.includes('<type>16th</type>'),
    'WASM MusicXML 里不再补出假的 16 分休止');
  check(ggXml.includes('<note><rest/><duration>480</duration>'),
    'WASM MusicXML 保留真四分休止');

  const ggHtml = gg.call('midi2jianpu_html', -1, 1) || '';
  check((ggHtml.match(/class='note quarter'/g) || []).length === 8,
    'WASM HTML 仍是 8 个四分音符列（7 音 + 1 休止）');
}

// ---- 52. v1.55.0 复调轨按声部补空档（网页端同一份 core）----
// poly_gap.mid：左手按住全音符让整轨判成复调，右手是 DAW 门限 85% 的
// 四分音符。修复前文本谱一小节只剩 3 拍、MusicXML 补出 7 个 16 分休止，
// 网页端读同一份 core，必须一起变成四个四分音符，左手声部一字不改。
const pgPath = path.join(FIXTURES, 'poly_gap.mid');
if (existsSync(pgPath)) {
  const pg = makeCaller(mod);
  pg.setMidi(new Uint8Array(readFileSync(pgPath)));
  pg.setLyrics('');
  const pgText = pg.call('midi2jianpu_parse', -1, 1) || '';
  check(pgText.includes('1 2 3 4  |'),
    'WASM 文本谱右手第 1 小节读作四个四分音符（旧版只有 3 拍）', pgText);
  check(pgText.includes("5 6 7 1'  |"),
    'WASM 文本谱右手第 2 小节同样读作 4 拍');
  check(!pgText.includes('1_. 2_. 3_. 4_.'),
    'WASM 文本谱不再写成「附点八分」串');
  check(pgText.includes('1, - - -  |'),
    'WASM 文本谱左手全音符仍单独成声部、时值一字不改');

  const pgXml = pg.call('midi2jianpu_musicxml', -1, 1) || '';
  check(!pgXml.includes('<type>16th</type>'),
    'WASM MusicXML 里不再补出假的 16 分休止');
  check(!pgXml.includes('<rest/>'),
    'WASM MusicXML 里不再补出任何假休止');

  const pgHtml = pg.call('midi2jianpu_html', -1, 1) || '';
  check((pgHtml.match(/class='note quarter'/g) || []).length === 8,
    'WASM HTML 是 8 个四分音符列');
  check(pgHtml.includes("class='note whole'"),
    'WASM HTML 保留左手的全音符列');
}

// ---- 53. v1.56.0 复调轨门限因子按声部估（网页端同一份 core）----
// poly_gate.mid：左手 24 个八分连奏音把整轨 dur/ioi 众数锁死在 1.0，
// 右手门限 70% 的 12 个四分音符全被写成附点八分、MusicXML 补出 12 个
// 16 分休止。网页端读同一份 core，必须一起变成四分音符，左手一字不改。
const pgatePath = path.join(FIXTURES, 'poly_gate.mid');
if (existsSync(pgatePath)) {
  const pgt = makeCaller(mod);
  pgt.setMidi(new Uint8Array(readFileSync(pgatePath)));
  pgt.setLyrics('');
  const pgtText = pgt.call('midi2jianpu_parse', -1, 1) || '';
  check(pgtText.includes('1 2 3 4  |'),
    'WASM 文本谱右手第 1 小节读作四个四分音符（旧版是附点八分）', pgtText);
  check(pgtText.includes("5 6 7 1'  |"),
    'WASM 文本谱右手第 2 小节同样读作 4 拍');
  check(pgtText.includes('7 6 5 4  |'),
    'WASM 文本谱右手第 3 小节同样读作 4 拍');
  check(!pgtText.includes('1_. 2_. 3_. 4_.'),
    'WASM 文本谱不再写成「附点八分」串');
  check(pgtText.includes('1,_ #1,_ 2,_'),
    'WASM 文本谱左手 24 个八分连奏音一字不改');
  check(!pgtText.includes('-_  |') && !pgtText.includes('-=  |'),
    'WASM 文本谱末音不被虚增、没有凭空多出第 4 小节');

  const pgtXml = pgt.call('midi2jianpu_musicxml', -1, 1) || '';
  check(!pgtXml.includes('<type>16th</type>'),
    'WASM MusicXML 里不再补出假的 16 分休止');
  check(!pgtXml.includes('<rest/>'),
    'WASM MusicXML 里不再补出任何假休止');

  const pgtHtml = pgt.call('midi2jianpu_html', -1, 1) || '';
  check((pgtHtml.match(/class='note quarter'/g) || []).length === 12,
    'WASM HTML 是 12 个四分音符列');
  check((pgtHtml.match(/class='note eighth'/g) || []).length === 24,
    'WASM HTML 是 24 个八分音符列');
}

// ---- 54. v1.58.0 休止符时值拆分（网页端同一份 core）----
// rest_split.mid：1.75 / 1.25 / 3.75 拍三段不规则休止必须拆成标准时值之和，
// 每个 <note><rest/> 的 <duration> 与 <type>/<dot/> 严格一致，总时长守恒。
const rsPath = path.join(FIXTURES, 'rest_split.mid');
if (existsSync(rsPath)) {
  const rst = makeCaller(mod);
  rst.setMidi(new Uint8Array(readFileSync(rsPath)));
  rst.setLyrics('');
  const rsText = rst.call('midi2jianpu_parse', -1, 1) || '';
  check(rsText.includes('1= 0 - - -_.  |') && rsText.includes('2 0 - -  |'),
    'WASM 文本谱不受拆分影响：前两小节仍各守恒 4 拍', rsText);
  check(rsText.includes('3= 0. -= 4= 0 -= 5_  |'),
    'WASM 最后一小节也拆段并守恒 4 拍（v1.59.0）', rsText);

  const rsXml = rst.call('midi2jianpu_musicxml', -1, 1) || '';
  check(rsXml.includes('<score-partwise'), 'WASM 导出 MusicXML');
  check(!rsXml.includes('<duration>840</duration>') &&
        !rsXml.includes('<duration>600</duration>') &&
        !rsXml.includes('<duration>1800</duration>'),
    'WASM 三段不规则间隙不再各写成一个近似休止符');
  check(rsXml.includes('<note><rest/><duration>720</duration><voice>1</voice>'
        + '<type>quarter</type><dot/></note>'),
    'WASM 1.75 拍拆出附点四分休止 720');
  check(rsXml.includes('<note><rest/><duration>360</duration><voice>1</voice>'
        + '<type>eighth</type><dot/></note>'),
    'WASM 3.75 拍拆出附点八分休止 360');

  const TP = { whole: 4, half: 2, quarter: 1, eighth: 0.5,
               '16th': 0.25, '32nd': 0.125, '64th': 0.0625 };
  const divM = rsXml.match(/<divisions>(\d+)<\/divisions>/);
  const div = divM ? Number(divM[1]) : 480;
  const restLines = (rsXml.match(/<note><rest\/>[^\n]*<\/note>/g) || []);
  let rsBad = 0;
  for (const line of restLines) {
    const dm = line.match(/<duration>(\d+)<\/duration>/);
    const tm = line.match(/<type>([^<]+)<\/type>/);
    if (!dm || !tm) continue;
    let want = TP[tm[1]] === undefined ? 1 : TP[tm[1]];
    if (line.includes('<dot/>')) want *= 1.5;
    if (Math.abs(Number(dm[1]) / div - want) > 1e-6) rsBad++;
  }
  check(rsBad === 0, 'WASM 每个休止符 duration 与 type/dot 严格一致',
    String(rsBad));
  check(restLines.length === 7, 'WASM 共 7 个休止符（6 段拆分 + 1 个精确对照）',
    String(restLines.length));
  const rsSums = [...rsXml.matchAll(/<measure[^>]*>([\s\S]*?)<\/measure>/g)]
    .map(m => [...m[1].matchAll(/<duration>(\d+)<\/duration>/g)]
      .reduce((a, d) => a + Number(d[1]), 0));
  check(JSON.stringify(rsSums) === JSON.stringify([1920, 1920, 1920]),
    'WASM 三个小节总时长都守恒为 1920 tick', JSON.stringify(rsSums));
}

// ---- 55. v1.59.0 小节中间休止符精确拆段（网页端同一份 core）----
// rest_midbar.mid：1.75 / 1.25 拍两段间隙夹在小节中间，后面紧跟音头。
// 旧版按最近档记成 0. / 0，第 1 小节读 3.5 拍；新版拆成 0. -= / 0 -=，
// 两小节都守恒 4 拍。网页端读同一份 core，必须一起修好。
const rmPath = path.join(FIXTURES, 'rest_midbar.mid');
if (existsSync(rmPath)) {
  const rm = makeCaller(mod);
  rm.setMidi(new Uint8Array(readFileSync(rmPath)));
  rm.setLyrics('');
  const rmText = rm.call('midi2jianpu_parse', -1, 1) || '';
  check(rmText.includes('1= 0. -= 2= 0 -= 3_  |'),
    'WASM 两段不规则休止精确拆成 0. -= / 0 -=', rmText);
  check(!rmText.includes('1= 0. 2= 0 3_'),
    'WASM 旧的按最近档近似写法不再出现', rmText);
  check(rmText.includes('4 5 6 7  |'),
    'WASM 第 2 小节四个四分音符逐字节不变', rmText);

  const rmXml = rm.call('midi2jianpu_musicxml', -1, 1) || '';
  const rmSums = [...rmXml.matchAll(/<measure[^>]*>([\s\S]*?)<\/measure>/g)]
    .map(m => [...m[1].matchAll(/<duration>(\d+)<\/duration>/g)]
      .reduce((a, d) => a + Number(d[1]), 0));
  check(JSON.stringify(rmSums) === JSON.stringify([1920, 1920]),
    'WASM 两小节总时长都守恒 1920 tick', JSON.stringify(rmSums));

  const rmHtml = rm.call('midi2jianpu_html', -1, 1) || '';
  check(rmHtml.includes("class='note sixteenth'"),
    'WASM HTML 十六分音符排版不受休止拆段影响', rmHtml.slice(-200));
}


// ---- 56. v1.60.0 HTML 谱面附点标记（网页端同一份 core）----
// html_dots.mid 把 0.375 / 0.75 / 1.5 拍的附点单音、附点休止符、柱式和弦
// 与 3 拍延音线长音放在同一份素材里。旧版 HTML 一个 dotted class 都没有，
// 网页端读同一份 core，必须一起把 CSS 的 .dotted .num::after 接上。
const hdpPath = path.join(FIXTURES, 'html_dots.mid');
if (existsSync(hdpPath)) {
  const hdp = makeCaller(mod);
  hdp.setMidi(new Uint8Array(readFileSync(hdpPath)));
  hdp.setLyrics('');
  const hdpText = hdp.call('midi2jianpu_parse', -1, 1) || '';
  check(hdpText.includes('1_. 2=. 3=. 4. 5  |'),
    'WASM 文本谱附点记号不变', hdpText);
  check(hdpText.includes('0_.'),
    'WASM 文本谱保留附点八分休止', hdpText);

  const hdpHtml = hdp.call('midi2jianpu_html', -1, 1) || '';
  check((hdpHtml.match(/class='note eighth dotted'/g) || []).length === 3,
    'WASM HTML 两处 0.75 拍单音与附点八分休止都画点');
  check((hdpHtml.match(/class='note sixteenth dotted'/g) || []).length === 2,
    'WASM HTML 两个 0.375 拍画附点十六分');
  check((hdpHtml.match(/class='note quarter dotted'/g) || []).length === 2,
    'WASM HTML 1.5 拍单音与和弦都画附点四分');
  check((hdpHtml.match(/class='tone dotted'/g) || []).length === 3,
    'WASM HTML 三串柱式和弦各只画一个点（挂在最低音）');
  check(!hdpHtml.includes("class='note chordcol eighth dotted'"),
    'WASM HTML 和弦整列不挂 dotted');
  check(!hdpHtml.includes("class='note half dotted'") &&
        !hdpHtml.includes("class='note whole dotted'"),
    'WASM HTML 3 拍 / 6 拍靠延音线表达，不画附点');

  const hdpXml = hdp.call('midi2jianpu_musicxml', -1, 1) || '';
  check((hdpXml.match(/<dot\/>/g) || []).length >= 16,
    'WASM MusicXML 仍有 <dot/> 作真值');
}


// ---- 57. v1.61.0 HTML 休止符精确拆列（网页端同一份 core）----
// html_rests.mid 把 1.75 / 1.25 / 2.5 / 2.25 拍四段不规则间隙与
// 0.75 / 1.0 拍两个精确档放在同一份素材里。旧版 HTML 用 durStyleOf()
// 的最近档把 1.75 拍画成 1.5 拍、1.25 拍画成 1 拍，网页端读同一份
// core，必须一起按精确档位之和拆成多列。
const hrpPath = path.join(FIXTURES, 'html_rests.mid');
if (existsSync(hrpPath)) {
  const hrp = makeCaller(mod);
  hrp.setMidi(new Uint8Array(readFileSync(hrpPath)));
  hrp.setLyrics('');
  const hrpText = hrp.call('midi2jianpu_parse', -1, 1) || '';
  check(hrpText.includes('0. -= 2= 0 -= 3_  |'),
    'WASM 文本谱 1.75 / 1.25 拍精确拆段不变', hrpText);
  check(hrpText.includes('0 - -_ 6=') && hrpText.includes("0 - -= 1'= 0"),
    'WASM 文本谱 2.5 / 2.25 拍精确拆段不变', hrpText);

  const hrpHtml = hrp.call('midi2jianpu_html', -1, 1) || '';
  check((hrpHtml.match(/<span class='num'>0<\/span>/g) || []).length === 10,
    'WASM HTML 四段拆列 + 两个精确档共 10 个 0 列');
  const hrp16 = "<span class='note sixteenth'><span class='oct'></span>" +
                "<span class='core'><span class='num'>0</span>";
  check(hrpHtml.split(hrp16).length - 1 === 3,
    'WASM HTML 1.75 / 1.25 / 2.25 拍各补一个十六分休止续列');
  const hrp8 = "<span class='note eighth'><span class='oct'></span>" +
               "<span class='core'><span class='num'>0</span>";
  check(hrpHtml.split(hrp8).length - 1 === 1,
    'WASM HTML 2.5 拍第二段是八分休止列');
  check((hrpHtml.match(/--n:1/g) || []).length === 2,
    'WASM HTML 2.5 / 2.25 拍首段是二分休止');
  check((hrpHtml.match(/class='note eighth dotted'/g) || []).length === 1,
    'WASM HTML 0.75 拍精确档仍是单列附点八分');

  const hrpXml = hrp.call('midi2jianpu_musicxml', -1, 1) || '';
  check((hrpXml.match(/<rest/g) || []).length === 10,
    'WASM MusicXML 也是 10 个 rest note，与 HTML 逐列对应');
}

// ---- 58. v1.70.0 大谱表跨谱表楔形：WASM 起止落同一谱表 ----
// 旧产物每条 <direction> 的 <staff> 各自按拍位取：渐强起点在上行谱、终点
// 在下行谱，hairpin 被拆成两段断裂的楔形。修复后以起点谱表为锚，stop 与
// 起音共用同一 <staff>，整条楔形留在同一行谱。
const gswPath = path.join(FIXTURES, 'grand_staff_wedge.mid');
if (existsSync(gswPath)) {
  mod._midi2jianpu_set_track(-1);
  const gsw = makeCaller(mod);
  gsw.setMidi(new Uint8Array(readFileSync(gswPath)));
  gsw.setLyrics('');
  const gswXml = gsw.call('midi2jianpu_musicxml', -1, 1) || '';
  check(/<staves>2<\/staves>/.test(gswXml),
    'WASM 跨谱表楔形素材被判为大谱表');
  const wstaffs = [];
  for (const blk of gswXml.match(/<direction\b[^>]*>[\s\S]*?<\/direction>/g) || []) {
    if (blk.includes('<wedge')) {
      const m = blk.match(/<staff>(\d+)<\/staff>/);
      wstaffs.push(m ? m[1] : null);
    }
  }
  check(wstaffs.length === 2 && wstaffs[0] === '1' && wstaffs[1] === '1',
    'WASM crescendo 起与 stop 止都落 staff 1（旧产物 stop 落 staff 2），实际 ' +
    JSON.stringify(wstaffs));
}

// ---- 59. v1.80.0 旧编码 MIDI：WASM 与 CLI 走同一条 UTF-8 归一 ----
// 文本 meta 是裸字节。网页端把 .mid 读成 Uint8Array 直接喂进 WASM，若解析层
// 不归一，UTF8ToString 会把 GBK / Shift-JIS 字节变成 U+FFFD；MusicXML 更是
// 整份打不开。三份素材同源，GBK 版与 UTF-8 版的 MusicXML 必须逐字节一致。
const lgPath = path.join(FIXTURES, 'legacy_gbk.mid');
const luPath = path.join(FIXTURES, 'legacy_utf8.mid');
const lsPath = path.join(FIXTURES, 'legacy_sjis.mid');
if (existsSync(lgPath) && existsSync(luPath) && existsSync(lsPath)) {
  mod._midi2jianpu_set_track(-1);
  const lc = makeCaller(mod);
  lc.setLyrics('');
  lc.setMidi(new Uint8Array(readFileSync(lgPath)));
  const lcgXml = lc.call('midi2jianpu_musicxml', -1, 1) || '';
  const lcgHtml = lc.call('midi2jianpu_html', -1, 1) || '';
  lc.setMidi(new Uint8Array(readFileSync(luPath)));
  const lcuXml = lc.call('midi2jianpu_musicxml', -1, 1) || '';
  lc.setMidi(new Uint8Array(readFileSync(lsPath)));
  const lcsXml = lc.call('midi2jianpu_musicxml', -1, 1) || '';
  check(lcgXml === lcuXml && lcgXml.includes('副歌段落'),
    'WASM 把 GBK 文本 meta 归一后，与 UTF-8 对照素材的 MusicXML 逐字节一致');
  check(lcgHtml.includes('主旋律 钢琴') && !lcgHtml.includes('\uFFFD'),
    'WASM HTML 的中文轨名正常，没有替换字符');
  check(lcsXml.includes('サビ段落') && lcsXml.includes('の') &&
    !lcsXml.includes('\uFFFD'),
    'WASM 也能解 Shift-JIS 的假名段落名与歌词');
}

// ---- 60. v1.86.0 越界点事件：网页端也不再谎报变拍号 / 变速 / 转调 ----
// CLI 冒烟 [99] 覆盖三端，但网页跑的是 WASM 编译出来的同一份 midifile.cpp。
// 这里端到端确认 docs/wasm/ 里的产物确实带上了修复（漏重编就会红）：
// 1 小节 4 个音的谱面页眉必须如实提示「已丢弃 N 个越界事件」，而不是
// 「4/4 → 3/4(第11小节)」「♩=120 → 256」「1=C → 1=D」「·小提琴」。
const sePath = path.join(FIXTURES, 'stray_events.mid');
if (existsSync(sePath)) {
  mod._midi2jianpu_set_track(-1);
  const sc = makeCaller(mod);
  sc.setLyrics('');
  sc.setMidi(new Uint8Array(readFileSync(sePath)));
  const seHtml = sc.call('midi2jianpu_html', -1, 1) || '';
  const seXml = sc.call('midi2jianpu_musicxml', -1, 1) || '';
  check(seHtml.includes('已丢弃') && seHtml.includes('时间轴跨度异常'),
    'WASM HTML 页眉如实提示已丢弃越界事件', seHtml.slice(0, 300));
  check(!seHtml.includes('→ 3/4') && !seHtml.includes('→ 256') &&
        !seHtml.includes('→ 1=D') && !seHtml.includes('小提琴'),
    'WASM HTML 不再谎报变拍号 / 变速 / 转调 / 乐器名', seHtml.slice(0, 300));
  check(seHtml.includes('1 2 3 4') || seHtml.includes('1</span>'),
    'WASM 仍照常输出 4 个界内音符', seHtml.slice(0, 300));
  check(seHtml.length < 1024 * 1024 && seXml.length < 1024 * 1024,
    'WASM HTML / MusicXML 体积有界');
}

// ---- 61. v1.87.0 解析期事件过闸：网页端也不铺幽灵长音、不报幽灵诊断 ----
// 踏板 / SysEx / FF21 / CC7 在解析循环里就被消费掉，漏重编 WASM 就会让
// 网页端继续铺 256 个空小节、页眉凭空报出「SysEx 鼓模式」「MIDI Port」等。
const v87PedPath = path.join(FIXTURES, 'stray_pedal.mid');
const v87DiagPath = path.join(FIXTURES, 'stray_diag.mid');
if (existsSync(v87PedPath) && existsSync(v87DiagPath)) {
  mod._midi2jianpu_set_track(-1);
  const v87Caller = makeCaller(mod);
  v87Caller.setLyrics('');
  v87Caller.setMidi(new Uint8Array(readFileSync(v87PedPath)));
  const v87PedHtml = v87Caller.call('midi2jianpu_html', -1, 1) || '';
  check(v87PedHtml.includes('已丢弃') && v87PedHtml.includes('时间轴跨度异常'),
    'WASM 页眉如实提示已丢弃越界抬踏板', v87PedHtml.slice(0, 300));
  check(!v87PedHtml.includes('已截断') && !v87PedHtml.includes('弱起 3 拍'),
    'WASM 不再把音符夹成幽灵长音、也不谎报弱起', v87PedHtml.slice(0, 300));
  check(v87PedHtml.length < 30000,
    'WASM HTML 体积有界（旧版约 615 KB）', v87PedHtml.length);
  v87Caller.setMidi(new Uint8Array(readFileSync(v87DiagPath)));
  const v87DiagHtml = v87Caller.call('midi2jianpu_html', -1, 1) || '';
  for (const ghost of ['SysEx 鼓模式', 'MIDI Port', '通道前缀',
                       'CC7 主音量', '音色库/音色设定', '小提琴']) {
    check(!v87DiagHtml.includes(ghost), `WASM 页眉不再凭空报出 ${ghost}`,
      v87DiagHtml.slice(0, 300));
  }
  check(v87DiagHtml.includes('已丢弃') && v87DiagHtml.includes('钢琴'),
    'WASM 诊断素材仍保留界内钢琴音色并如实提示', v87DiagHtml.slice(0, 300));
}

// ---- 62. v1.88.0 护栏收口：网页端不再因合法极晚长音铺 6000 个空小节 ----
// CLI 冒烟 [101] 覆盖三端，这里端到端确认 docs/wasm/ 里的 .wasm 确实带上
// 512 小节上限（漏重编就会是 2.46 MB HTML + 「已截断 2 个」）。
const v88LatePath = path.join(FIXTURES, 'late_note.mid');
if (existsSync(v88LatePath)) {
  mod._midi2jianpu_set_track(-1);
  const v88Caller = makeCaller(mod);
  v88Caller.setLyrics('');
  v88Caller.setMidi(new Uint8Array(readFileSync(v88LatePath)));
  const v88Html = v88Caller.call('midi2jianpu_html', -1, 1) || '';
  const v88Xml = v88Caller.call('midi2jianpu_musicxml', -1, 1) || '';
  check(v88Html.includes('已截断 1 个越界音符') && v88Html.includes('时间轴跨度异常'),
    'WASM 页眉如实提示已截断 1 个越界音符', v88Html.slice(0, 300));
  check(!v88Html.includes('已截断 2 个'),
    'WASM 不再把同一个音重复计成 2 个', v88Html.slice(0, 300));
  check(v88Html.length < 30000 && v88Xml.length < 30000,
    'WASM HTML / MusicXML 体积有界（旧版 2.46 MB / 888 KB）',
    v88Html.length + ' / ' + v88Xml.length);
  check(v88Html.includes('1 2 3 4') || v88Html.includes('1</span>'),
    'WASM 仍照常输出 4 个界内音符', v88Html.slice(0, 300));
}

// ---- 63. v1.89.0 护栏与拍号解耦：网页端同样不误杀、不放大 ----
// CLI 冒烟 [102] 覆盖三端，这里端到端确认 docs/wasm/ 里的 .wasm 确实带上
// 「固定 4/4 基准 cap + 离谱拍号校验」（漏重编就会是 128 音 / 49 MB HTML）。
const v89ShortPath = path.join(FIXTURES, 'edge', 'short_meter.mid');
const v89AbsurdPath = path.join(FIXTURES, 'absurd_meter.mid');
if (existsSync(v89ShortPath) && existsSync(v89AbsurdPath)) {
  mod._midi2jianpu_set_track(-1);
  const v89Caller = makeCaller(mod);
  v89Caller.setLyrics('');
  v89Caller.setMidi(new Uint8Array(readFileSync(v89ShortPath)));
  const v89ShortHtml = v89Caller.call('midi2jianpu_html', -1, 1) || '';
  check(!v89ShortHtml.includes('已截断') &&
        !v89ShortHtml.includes('时间轴跨度异常'),
    'WASM 短拍号长曲不再谎报截断 / 时间轴异常', v89ShortHtml.slice(0, 300));
  check(v89ShortHtml.includes('1/16') && v89ShortHtml.includes('4/4'),
    'WASM 仍照常显示合法的 1/16 → 4/4 变拍号', v89ShortHtml.slice(0, 300));
  check((v89ShortHtml.match(/class='note/g) || []).length > 100,
    'WASM 短拍号长曲的 160 个音照常出谱',
    (v89ShortHtml.match(/class='note/g) || []).length);

  v89Caller.setMidi(new Uint8Array(readFileSync(v89AbsurdPath)));
  const v89AbsurdHtml = v89Caller.call('midi2jianpu_html', -1, 1) || '';
  const v89AbsurdXml = v89Caller.call('midi2jianpu_musicxml', -1, 1) || '';
  check(!v89AbsurdHtml.includes('255/1') && !v89AbsurdHtml.includes('1/256'),
    'WASM 页眉不再出现离谱拍号', v89AbsurdHtml.slice(0, 300));
  check(v89AbsurdHtml.length < 300000 && v89AbsurdXml.length < 150000,
    'WASM HTML / MusicXML 体积有界（旧版 49.5 MB / 5.35 MB）',
    v89AbsurdHtml.length + ' / ' + v89AbsurdXml.length);
  check(v89AbsurdHtml.includes('已截断 1 个越界音符'),
    'WASM 仍如实提示极长音被截断', v89AbsurdHtml.slice(0, 300));
}
// ---- 64. v1.90.0 页眉转义：网页端同样不注入可执行脚本 ----
// 网页端走同一个 html_writer.cpp；漏重编 docs/wasm/ 时旧 .wasm 仍会
// 把 0x03 轨名原样拼进 <div class="meta">，这一组能直接抓到。
const v90InjectPath = path.join(FIXTURES, 'html_meta_inject.mid');
if (existsSync(v90InjectPath)) {
  mod._midi2jianpu_set_track(-1);
  const v90Caller = makeCaller(mod);
  v90Caller.setLyrics('');
  v90Caller.setMidi(new Uint8Array(readFileSync(v90InjectPath)));
  const v90Html = v90Caller.call('midi2jianpu_html', -1, 1) || '';
  check(!v90Html.includes('<script>alert(1)</script>'),
    'WASM HTML 不含可执行的注入脚本', v90Html.slice(0, 300));
  check(v90Html.includes('&lt;script&gt;alert(1)&lt;/script&gt;'),
    'WASM 页眉轨名转义成实体', v90Html.slice(0, 300));
  check(v90Html.includes('A&amp;B') && !v90Html.includes('A&amp;amp;B'),
    'WASM 的 & 只转义一次，没有双重转义', v90Html.slice(0, 300));
  check((v90Html.match(/<script/g) || []).length === 0,
    'WASM 未开播放器时整份页面没有 script 标签', v90Html.slice(0, 300));
  check(v90Html.includes('class=\'note'),
    'WASM 转义后谱面音符照常出谱', v90Html.slice(0, 300));
}

// ---- 65. v1.100.0 外部歌词时间轴越界：网页端也不把歌词挂到曲首 ----
// 网页端的 set_lyrics 走同一个 parseLrcText；漏重编 docs/wasm/ 时旧 .wasm
// 仍会把 [offset:999999999999] 环绕后的假 tick 当真。
if (existsSync(v90InjectPath)) {
  const v100Caller = makeCaller(mod);
  v100Caller.setMidi(new Uint8Array(readFileSync(v90InjectPath)));
  v100Caller.setLyrics('[00:00.00]你好\n[00:01.00]世界\n');
  const v100OkHtml = v100Caller.call('midi2jianpu_html', -1, 1) || '';
  check(v100OkHtml.includes("<span class='lyric'>你</span>") &&
        v100OkHtml.includes("<span class='lyric'>世</span>"),
    'WASM 正常 LRC 两个词都出谱', v100OkHtml.slice(0, 300));
  v100Caller.setLyrics('[offset:999999999999]\n[00:00.00]你好\n[00:01.00]世界\n');
  const v100BadHtml = v100Caller.call('midi2jianpu_html', -1, 1) || '';
  check(!v100BadHtml.includes("<span class='lyric'>世</span>") &&
        !v100BadHtml.includes("<span class='lyric'>你</span>"),
    'WASM 越界 LRC 不再把歌词错挂到曲首', v100BadHtml.slice(0, 300));
  check(v100BadHtml.includes('class=\'note'),
    'WASM 越界歌词不影响音符照常出谱', v100BadHtml.slice(0, 300));
  v100Caller.setLyrics('[99999999:00.00]你好\n');
  const v100StampHtml = v100Caller.call('midi2jianpu_html', -1, 1) || '';
  check(!v100StampHtml.includes("<span class='lyric'>你</span>"),
    'WASM 巨大时间戳同样不产出错挂歌词', v100StampHtml.slice(0, 300));
  v100Caller.setLyrics('');
}

// ---- 66. v1.102.0 LRC 首行 UTF-8 BOM：网页端也不静默丢词 ----
// set_lyrics 传进来的 JS 字符串由 stringToUTF8 编成 EF BB BF 开头；
// parseLrcTextImpl 要求 line[0]=='['，旧版首行整行被跳过。
if (existsSync(v90InjectPath)) {
  const v102Caller = makeCaller(mod);
  v102Caller.setMidi(new Uint8Array(readFileSync(v90InjectPath)));
  v102Caller.setLyrics('\uFEFF[00:00.00]你好\n[00:01.00]世界\n');
  const v102BomHtml = v102Caller.call('midi2jianpu_html', -1, 1) || '';
  check(v102BomHtml.includes("<span class='lyric'>你</span>") &&
        v102BomHtml.includes("<span class='lyric'>世</span>"),
    'WASM LRC 首行带 UTF-8 BOM 两行词都出谱', v102BomHtml.slice(0, 300));
  check(!v102BomHtml.includes('\uFFFD'),
    'WASM BOM 不产生 U+FFFD 替换字符', v102BomHtml.slice(0, 300));
  v102Caller.setLyrics('[00:00.00]你好\n[00:01.00]世界\n');
  const v102PlainHtml = v102Caller.call('midi2jianpu_html', -1, 1) || '';
  check(v102PlainHtml.includes("<span class='lyric'>你</span>") &&
        v102PlainHtml.includes("<span class='lyric'>世</span>"),
    'WASM 无 BOM 对照仍逐字节一致', v102PlainHtml.slice(0, 300));
  v102Caller.setLyrics('');
}

// ---- 67. v1.103.0 外部歌词控制字符：网页端也不再印裸 C0 / DEL ----
// set_lyrics 传进来的是 JS 字符串（已是 Unicode），不做字节转码；但纯文本
// 入口的 sanitizeLyricControls 对三端一视同仁。MusicXML 的 xmlEscape() 早就
// 会丢 C0，HTML 页眉转义却不会 —— 同一份词在网页端 HTML 里留下非法控制字节。
if (existsSync(v90InjectPath)) {
  const v103Caller = makeCaller(mod);
  v103Caller.setMidi(new Uint8Array(readFileSync(v90InjectPath)));
  v103Caller.setLyrics('[00:00.00]你\u0001\u007F好\n[00:01.00]世界\n');
  const v103Html = v103Caller.call('midi2jianpu_html', -1, 1) || '';
  check(v103Html.includes("<span class='lyric'>你</span>") &&
        v103Html.includes("<span class='lyric'>好</span>"),
    'WASM LRC 里的裸 C0/DEL 被过滤，中文仍完整出谱', v103Html.slice(0, 300));
  check(!/[\u0000-\u0008\u000B\u000C\u000E-\u001F\u007F]/.test(v103Html),
    'WASM HTML 不含任何裸 C0 / DEL 控制字符', v103Html.slice(0, 300));
  const v103Xml = v103Caller.call('midi2jianpu_musicxml', -1, 1) || '';
  check(!/[\u0000-\u0008\u000B\u000C\u000E-\u001F\u007F]/.test(v103Xml),
    'WASM MusicXML 同样不含裸控制字符', v103Xml.slice(0, 300));
  v103Caller.setLyrics('\uFEFF[00:00.00]你好\n[00:01.00]世界\n');
  const v103Bom = v103Caller.call('midi2jianpu_html', -1, 1) || '';
  check(v103Bom.includes("<span class='lyric'>你</span>") &&
        v103Bom.includes("<span class='lyric'>世</span>"),
    'WASM UTF-8 BOM 首行仍不丢词（v1.102.0 零回归）', v103Bom.slice(0, 300));
  v103Caller.setLyrics('[00:00.00]你好\n[00:01.00]世界\n');
  const v103Plain = v103Caller.call('midi2jianpu_html', -1, 1) || '';
  check(v103Plain.includes("<span class='lyric'>你</span>") &&
        v103Plain.includes("<span class='lyric'>世</span>"),
    'WASM 正常 LRC 逐字节零回归', v103Plain.slice(0, 300));
  v103Caller.setLyrics('');
}

// ---- 68. v1.104.0 网页端歌词上传字节解码：覆盖 GBK / UTF-16 / UTF-8 ----
// 旧版网页用 FileReader.readAsText(f) 默认 UTF-8 读上传歌词，GBK / UTF-16 在
// 进 WASM 前就被浏览器折成 U+FFFD；v1.104.0 改为 readAsArrayBuffer 取原始字节，
// 交给新增的 _midi2jianpu_decode_lyric_file（包装 CLI 的 normalizeLyricFileBytes，
// 与 CLI 读盘入口同款解码）。本组直接调该导出，断言各编码正确还原中文、无 U+FFFD。
{
  const fn = mod._midi2jianpu_decode_lyric_file;
  check(typeof fn === 'function', 'WASM 导出 _midi2jianpu_decode_lyric_file 存在');
  function hexBytes(hex) {
    const out = new Uint8Array(hex.length / 2);
    for (let i = 0; i < out.length; i++) out[i] = parseInt(hex.substr(i * 2, 2), 16);
    return out;
  }
  function decodeBytes(hex) {
    const bytes = hexBytes(hex);
    const ptr = mod._malloc(bytes.length);
    mod.HEAPU8.set(bytes, ptr);
    const sptr = fn(ptr, bytes.length);
    const txt = sptr ? mod.UTF8ToString(sptr) : '';
    mod._free(ptr);
    return txt;
  }
  const gbk = decodeBytes('C4E3BAC3');            // GBK 你好
  check(gbk === '你好', 'GBK 字节解码为「你好」', gbk);
  check(!gbk.includes('�'), 'GBK 解码无 U+FFFD', gbk);

  const u16 = decodeBytes('FFFE604F7D59');        // UTF-16LE BOM + 你好
  check(u16 === '你好', 'UTF-16LE BOM 解码为「你好」', u16);
  check(!u16.includes('�'), 'UTF-16LE 解码无 U+FFFD', u16);

  const u8 = decodeBytes('E4BDA0E5A5BD');         // UTF-8 你好
  check(u8 === '你好', 'UTF-8 字节解码为「你好」', u8);

  const u8bom = decodeBytes('EFBBBFE4BDA0E5A5BD'); // UTF-8 BOM + 你好
  check(u8bom.includes('你好') && !u8bom.includes('�'),
    'UTF-8 BOM 解码含「你好」且无 U+FFFD', u8bom);

  const u32 = decodeBytes('FFFE0000');            // UTF-32LE BOM（无内容）
  check(u32 === '', 'UTF-32LE BOM 空内容返回空串', u32);

  // v1.107.0：无 BOM 的 UTF-16（NUL 是合法 UTF-8，旧版被原样放过 -> 时间戳失效）
  const lrcLe = '5B00300030003A00300030002E00300030005D00604F7D59';
  const noBomLe = decodeBytes(lrcLe);             // [00:00.00]你好
  check(noBomLe === '[00:00.00]你好', 'UTF-16LE 无 BOM 解码为 LRC 原文', noBomLe);
  const lrcBe = '005B00300030003A00300030002E00300030005D4F60597D';
  const noBomBe = decodeBytes(lrcBe);
  check(noBomBe === '[00:00.00]你好', 'UTF-16BE 无 BOM 解码为 LRC 原文', noBomBe);
  const noBomOdd = decodeBytes('5B00300030003A00300030002E00300030005D00604F7D');
  check(noBomOdd === '[00:00.00]你',
    '奇数长度的无 BOM UTF-16 忽略残缺尾字节', noBomOdd);
}

// ---- 69. v1.106.0 网页端歌词 dropped 计数可见：越界时间戳不再静默丢弃 ----
// CLI 在歌词时间戳超出谱面可解释范围时打印「警告: 已忽略 N 行歌词」，网页此前
// 在渲染路径里直接丢弃 dropped（0 行可解析时还会回退 MIDI 内置「LY」音节），
// 用户只会以为程序漏了。新增 _midi2jianpu_lyric_dropped(midi,size,lyrics) 与 CLI
// 同口径（都来自 parseLrcText 的 dropped）。本组直接调该导出验证计数。
if (existsSync(v90InjectPath)) {
  const fn = mod._midi2jianpu_lyric_dropped;
  check(typeof fn === 'function', 'WASM 导出 _midi2jianpu_lyric_dropped 存在');
  // 该导出按全局 g_trackFilter 解析；先复位到全部轨，保证计数与轨道选择无关。
  if (mod._midi2jianpu_set_track) mod._midi2jianpu_set_track(-1);
  const injMidi = new Uint8Array(readFileSync(v90InjectPath));
  const injPtr = mod._malloc(injMidi.length);
  mod.HEAPU8.set(injMidi, injPtr);
  function droppedOf(lyr) {
    const n = mod.lengthBytesUTF8(lyr) + 1;
    const lp = mod._malloc(n);
    mod.stringToUTF8(lyr, lp, n);
    const d = fn(injPtr, injMidi.length, lp);
    mod._free(lp);
    return d;
  }
  const normalDrop = droppedOf('[00:00.00]你好\n[00:01.00]世界\n');
  check(normalDrop === 0, 'WASM 正常 LRC 的 dropped 计数为 0', normalDrop);
  const offsetDrop = droppedOf('[offset:999999999999]\n[00:00.00]你好\n[00:01.00]世界\n');
  check(offsetDrop === 2, 'WASM 越界 offset 两行都计入 dropped', offsetDrop);
  const mixedDrop = droppedOf('[00:00.00]你好\n[99999999:00.00]世界\n');
  check(mixedDrop === 1, 'WASM 混合时间戳只把越界那行计入 dropped', mixedDrop);
  const emptyDrop = droppedOf('');
  check(emptyDrop === 0, 'WASM 空歌词的 dropped 计数为 0', emptyDrop);
  mod._free(injPtr);
}

// ---- 70. v1.108.0 同时间戳多行歌词：网页贴双语 LRC 时原文不再整行消失 ----
// 网页的歌词输入框直接吃 LRC 文本（set_lyrics -> parseLrcText），所以「原文一行、
// 译文一行共用一个 [mm:ss.xx]」这条路径在浏览器里同样会撞上 assignLyrics 的同戳
// 丢行缺陷。这里断言两行的字都进了谱面。
if (existsSync(v90InjectPath)) {
  const c70 = makeCaller(mod);
  c70.setMidi(new Uint8Array(readFileSync(v90InjectPath)));
  c70.setLyrics('[00:00.00]你好\n[00:00.00]Hello\n[00:02.00]再见\n');
  const h70 = c70.call('midi2jianpu_html', -1, 1) || '';
  check(h70.includes("<span class='lyric'>你</span>") &&
        h70.includes("<span class='lyric'>好</span>"),
    'WASM 双语同戳：原文两个字都出谱（旧版整行丢失）', h70.slice(0, 200));
  check(h70.includes("<span class='lyric'>H</span>") &&
        h70.includes("<span class='lyric'>再</span>"),
    'WASM 双语同戳：译文与下一句照常落字', h70.slice(0, 200));
  const x70 = c70.call('midi2jianpu_musicxml', -1, 1) || '';
  check(x70.includes('<text>你</text>') && x70.includes('<text>H</text>'),
    'WASM MusicXML 歌词与 HTML 同口径（原文在前）', x70.slice(0, 200));
  // 负向对照：单行歌词仍是逐字节等价的老口径。
  c70.setLyrics('[00:00.00]你好\n[00:02.00]再见\n');
  const h70b = c70.call('midi2jianpu_html', -1, 1) || '';
  check(h70b.includes("<span class='lyric'>你</span>") &&
        h70b.includes("<span class='lyric'>再</span>"),
    'WASM 单行歌词零回归', h70b.slice(0, 200));
  c70.setLyrics('');
}

// ---- 71. v1.109.0 LRC 时间戳宽进：冒号式百分秒 / 小时级 / 括号内空格 ----
// 网页歌词框直接吃 LRC 文本（set_lyrics -> parseLrcText），旧版只按第一个冒号
// 切一刀：冒号式百分秒慢掉两位、小时级归零堆到曲首、括号内空格整行不识。
// 这里按「歌词落在第几个音」逐条钉住——只断言「词出现过」是抓不到错位的。
if (existsSync(v90InjectPath)) {
  const c71 = makeCaller(mod);
  c71.setMidi(new Uint8Array(readFileSync(v90InjectPath)));
  // 每个音恰好一个 <span class='lyric'>（没词时是空的），线性顺序与音头一致：
  // 目标词之前已经有几个 lyric span，它就是第几个音。按 <span class='note 切段
  // 会被页面里 JS 模板字符串中的同类片段干扰（实测偏 +2）。
  function noteIndex70(html, ch) {
    const at = html.indexOf("class='lyric'>" + ch + '</span>');
    if (at < 0) return -1;
    return (html.slice(0, at).match(/class='lyric'>/g) || []).length;
  }
  c71.setLyrics('[ti:x]\n[00:00:00]甲\n[00:02:50]乙\n');
  const h71a = c71.call('midi2jianpu_html', -1, 1) || '';
  check(noteIndex70(h71a, '甲') === 0 && noteIndex70(h71a, '乙') === 5,
    'WASM 冒号式百分秒 [00:02:50] 落在第 6 个音（旧版第 5 个）',
    String(noteIndex70(h71a, '乙')));

  c71.setLyrics('[00:00:02.50]丙\n');
  const h71b = c71.call('midi2jianpu_html', -1, 1) || '';
  check(noteIndex70(h71b, '丙') === 5,
    'WASM 小时级 [00:00:02.50] 不再归零堆到曲首',
    String(noteIndex70(h71b, '丙')));

  c71.setLyrics('[ 00:01.00 ]丁\n');
  const h71c = c71.call('midi2jianpu_html', -1, 1) || '';
  check(noteIndex70(h71c, '丁') === 2,
    'WASM 括号内带空格的时间戳被接受（旧版 0 行，回退内置 LY）',
    String(noteIndex70(h71c, '丁')));

  c71.setLyrics('[00:02.00]戊\n');
  const h71d = c71.call('midi2jianpu_html', -1, 1) || '';
  check(noteIndex70(h71d, '戊') === 4,
    'WASM 规范写法落点零回归', String(noteIndex70(h71d, '戊')));
  c71.setLyrics('');
}

// ---- 72. v1.111.0 网页端也认 JSON 歌词（与 CLI 同一套格式判定）----
// 网页的 set_lyrics 原先写死 parseLrcText：把 CLI 那份
// [{"start_beat":N,"text":"…"}] 贴进歌词框，一行都取不到，页面静默回退成
// MIDI 内置的「LY」音节——同一份文件在命令行里是好好的。现在走
// detectLyricFormat("") 的内容嗅探，LRC / JSON 自动分流，三端一个口径。
if (existsSync(v90InjectPath)) {
  const c72 = makeCaller(mod);
  c72.setMidi(new Uint8Array(readFileSync(v90InjectPath)));
  // 第 71 组的 noteIndex70 是块作用域内的函数，这里另起一个同口径的
  const noteIndex72 = (html, ch) => {
    const at = html.indexOf("class='lyric'>" + ch + '</span>');
    if (at < 0) return -1;
    return (html.slice(0, at).match(/class='lyric'>/g) || []).length;
  };
  const jsonLy = '[{"start_beat":0,"text":"你"},{"start_beat":4,"text":"好"}]';

  c72.setLyrics(jsonLy);
  const h72 = c72.call('midi2jianpu_html', -1, 1) || '';
  check(h72.includes("<span class='lyric'>你</span>") &&
        h72.includes("<span class='lyric'>好</span>"),
    'WASM 贴 JSON 歌词能出词（旧版 0 行，回退内置 LY）', h72.slice(0, 200));
  check(!h72.includes("class='lyric'>LY"),
    'WASM 贴 JSON 歌词不再回退成 MIDI 内置的 LY 音节', h72.slice(0, 200));
  check(noteIndex72(h72, '你') === 0 && noteIndex72(h72, '好') === 4,
    'WASM JSON 的 start_beat 按 beat 换算后落在第 0 / 第 4 个音',
    `${noteIndex72(h72, '你')},${noteIndex72(h72, '好')}`);

  const x72 = c72.call('midi2jianpu_musicxml', -1, 1) || '';
  check(x72.includes('<text>你</text>') && x72.includes('<text>好</text>'),
    'WASM 导出 MusicXML 带同一份 JSON 歌词', x72.slice(0, 200));

  // 单字 JSON 也要落在它自己的 beat 上，别被当成整行铺开
  c72.setLyrics('[{"start_beat":2,"text":"甲"}]');
  const h72b = c72.call('midi2jianpu_html', -1, 1) || '';
  check(noteIndex72(h72b, '甲') === 2,
    'WASM 单字 JSON 落在 start_beat=2 对应的第 3 个音',
    String(noteIndex72(h72b, '甲')));

  // 越界 start_beat：与 CLI 一样计入 dropped、0 行时回退内置歌词
  if (mod._midi2jianpu_lyric_dropped) {
    const inj72 = new Uint8Array(readFileSync(v90InjectPath));
    const p72 = mod._malloc(inj72.length);
    mod.HEAPU8.set(inj72, p72);
    const n72 = mod.lengthBytesUTF8('[{"start_beat":999999,"text":"乙"}]') + 1;
    const lp72 = mod._malloc(n72);
    mod.stringToUTF8('[{"start_beat":999999,"text":"乙"}]', lp72, n72);
    const d72 = mod._midi2jianpu_lyric_dropped(p72, inj72.length, lp72);
    mod._free(lp72);
    mod._free(p72);
    check(d72 === 1, 'WASM JSON 的越界 start_beat 也计入 dropped', d72);
  }
  c72.setLyrics('[{"start_beat":999999,"text":"乙"}]');
  const h72c = c72.call('midi2jianpu_html', -1, 1) || '';
  check(h72c.includes("class='lyric'>LY"),
    'WASM 越界 start_beat 的 JSON 一行不剩，与 CLI 一样回退内置音节',
    h72c.slice(0, 160));

  // 畸形 JSON 不崩、不产生歌词（WASM 默认关异常，这里绝不能 abort 整页）
  c72.setLyrics('[{"start_beat":0,"text":');
  const h72d = c72.call('midi2jianpu_html', -1, 1) || '';
  check(h72d.includes('简谱') || h72d.includes('jpl'),
    'WASM 畸形 JSON 仍能出谱（不崩）', h72d.slice(0, 120));

  // 负向对照：LRC 仍走 LRC 分支，落点与旧版一致
  c72.setLyrics('[00:00.00]你好\n');
  const h72e = c72.call('midi2jianpu_html', -1, 1) || '';
  check(noteIndex72(h72e, '你') === 0 && noteIndex72(h72e, '好') === 4,
    'WASM LRC 仍按 LRC 解析，落点零回归',
    `${noteIndex72(h72e, '你')},${noteIndex72(h72e, '好')}`);
  c72.setLyrics('');
}

// ---- 73. v1.112.0 歌词体检：网页要知道「认了几个字 / 一行都没认出来」----
// 上一版只暴露 dropped 计数，于是「贴进去的歌词一个字都没认出来」这件事在
// 网页上完全看不见：谱面静默回退成 MIDI 内置的「LY」音节，提示栏一声不吭。
// 新导出 lyric_info 一次回「按哪种格式解析出几个字、忽略几行歌词 / 几个字」，
// 格式判定与解析仍在 C++（与 CLI 同一条路），网页不重写嗅探规则。v1.179.0
// 补：dropped 的计数单位也按格式分流，成因不得只写「越界时间戳」。
if (existsSync(v90InjectPath)) {
  const inj73 = new Uint8Array(readFileSync(v90InjectPath));
  check(typeof mod._midi2jianpu_lyric_info === 'function',
    'WASM 产物带 _midi2jianpu_lyric_info 导出（v1.112.0）',
    typeof mod._midi2jianpu_lyric_info);
  const lyricInfo = (text) => {
    const p = mod._malloc(inj73.length);
    mod.HEAPU8.set(inj73, p);
    const n = mod.lengthBytesUTF8(text) + 1;
    const lp = mod._malloc(n);
    mod.stringToUTF8(text, lp, n);
    const s = mod._midi2jianpu_lyric_info(p, inj73.length, lp);
    const out = s ? mod.UTF8ToString(s) : '';
    mod._free(lp);
    mod._free(p);
    return out;
  };

  check(lyricInfo('[00:00.00]你好\n[00:02.00]再见\n').includes('按 LRC 解析 4 字'),
    'lyric_info 认 LRC 并报出字数', lyricInfo('[00:00.00]你好\n[00:02.00]再见\n'));
  check(lyricInfo('[{"start_beat":0,"text":"你"},{"start_beat":1,"text":"好"}]')
          .includes('按 JSON 解析 2 字'),
    'lyric_info 认 JSON 并报出字数（格式判定与 CLI 同口径）',
    lyricInfo('[{"start_beat":0,"text":"你"},{"start_beat":1,"text":"好"}]'));
  check(lyricInfo('[{"start_beat":2,"text":"啊"}]').includes('按 JSON 解析 1 字'),
    'lyric_info 单字 JSON 计 1 字，不按字节数报 3',
    lyricInfo('[{"start_beat":2,"text":"啊"}]'));

  // 一个字都没认出来：必须明说（此前页面完全沉默）
  const noLyric = lyricInfo('这首歌的歌词\n没有时间戳\n');
  check(noLyric.includes('未识别出歌词行') && noLyric.includes('mm:ss'),
    'lyric_info 对无时间戳的纯文本明说「未识别出歌词行」并指出格式要求', noLyric);

  // 全都越界：LRC 按「行歌词」报，成因同时列出数值越界与条目格式错误
  const allOut = lyricInfo('[offset:999999999999]\n[00:00.00]你\n[999999:00.00]鬼\n');
  check(allOut.includes('2 行歌词全部被忽略') &&
        allOut.includes('数值超出谱面时间轴范围，或歌词条目格式错误'),
    'lyric_info 对 LRC 全部越界按「行歌词」报并列出两种成因', allOut);

  // v1.179.0：JSON 的每个元素就是一个字，dropped 不能说成「行」。
  const jsonAll = lyricInfo('[{"start_beat":999999,"text":"你"},'
                          + '{"start_beat":9999999,"text":"鬼"}]');
  check(jsonAll.includes('2 个字全部被忽略') && !jsonAll.includes('行'),
    'lyric_info 对 JSON 全部越界按「个字」报，不再说「行」', jsonAll);

  // start_beat=0 完全合法，但 text 不是字符串——dropped 的成因是格式错误，
  // 旧版却说「越界时间戳」，把用户指去改一个正确的时间戳。
  const jsonFmt = lyricInfo('[{"start_beat":0,"text":123}]');
  check(jsonFmt.includes('1 个字全部被忽略') && jsonFmt.includes('歌词条目格式错误'),
    'lyric_info 对 JSON 条目格式错误不再谎称「越界时间戳」', jsonFmt);

  // 部分越界：既报字数也按格式报忽略数
  const partOut = lyricInfo('[00:00.00]你\n[999999:00.00]鬼\n');
  check(partOut.includes('按 LRC 解析 1 字') && partOut.includes('忽略 1 行歌词'),
    'lyric_info LRC 部分越界时同时报字数与忽略行数', partOut);
  const jsonPart = lyricInfo('[{"start_beat":0,"text":"你"},'
                           + '{"start_beat":999999,"text":"鬼"}]');
  check(jsonPart.includes('按 JSON 解析 1 字') &&
        jsonPart.includes('忽略 1 个字') &&
        !jsonPart.includes('行越界时间戳'),
    'lyric_info JSON 部分越界按「忽略 1 个字」报', jsonPart);

  // 与 dropped 计数同口径：同一段文本两处报出来的丢弃数必须一致
  if (mod._midi2jianpu_lyric_dropped) {
    const droppedCount = (text) => {
      const p = mod._malloc(inj73.length);
      mod.HEAPU8.set(inj73, p);
      const n = mod.lengthBytesUTF8(text) + 1;
      const lp = mod._malloc(n);
      mod.stringToUTF8(text, lp, n);
      const d = mod._midi2jianpu_lyric_dropped(p, inj73.length, lp);
      mod._free(lp);
      mod._free(p);
      return d;
    };
    const txt = '[00:00.00]你\n[999999:00.00]鬼\n[9999999:00.00]神\n';
    check(droppedCount(txt) === 2 && lyricInfo(txt).includes('忽略 2 行歌词'),
      'lyric_info 的忽略行数与 lyric_dropped 一致', droppedCount(txt));
  }

  // 空 / 全空白：返回空串，网页不往提示栏塞废话
  check(lyricInfo('') === '' && lyricInfo('  \n\t ') === '',
    'lyric_info 对空歌词与全空白歌词返回空串',
    JSON.stringify([lyricInfo(''), lyricInfo('  \n\t ')]));

  // 静态守护：网页必须真的调用它，并且拖拽入口按扩展名 + MThd 魔数分流
  // （拖一个 .lrc 进来曾被当 MIDI 解析，页面直接报「无法解析MIDI文件」）。
  const page = readFileSync(path.join(ROOT, 'docs', 'index.html'), 'utf8');
  check(page.includes('_midi2jianpu_lyric_info(') &&
        page.includes('function handlePickedFile') &&
        page.includes('LYRIC_EXT') && page.includes('applyLyricBytes'),
    '网页接入 lyric_info，且拖拽按扩展名 + MThd 魔数分流歌词 / MIDI');
  // 旧 wasm 的兜底分支拿不到格式，只能用中性「条」，不能再把 JSON 说成「行」。
  check(!page.includes("' 行（时间戳超出谱面可解释范围）'") &&
        page.includes("' 条（数值超出谱面时间轴范围，或歌词条目格式错误）'"),
    '网页旧 wasm 兜底不再说「N 行（时间戳超出…）」');
}

// ---- 74. v1.113.0 挂不上的歌词字：网页也要说清「解析 N 字、挂上 M 字」----
// 一行字多于这段旋律的音头数时，多出来的字会被铺排丢掉；同戳行数多于音头时
// 后面几行整行消失。上一版 lyric_info 只报「解析 N 字」，用户以为谱面上真有
// N 个字，翻到那句却只剩半行。这里锁住三个数（解析 / 挂上 / 丢字）并把
// 「挂上几个」与页面上真印出来的 span 数对上，免得文案与渲染各说一套。
if (existsSync(v90InjectPath)) {
  const inj74 = new Uint8Array(readFileSync(v90InjectPath));
  const info74 = (text) => {
    const p = mod._malloc(inj74.length);
    mod.HEAPU8.set(inj74, p);
    const n = mod.lengthBytesUTF8(text) + 1;
    const lp = mod._malloc(n);
    mod.stringToUTF8(text, lp, n);
    const s = mod._midi2jianpu_lyric_info(p, inj74.length, lp);
    const out = s ? mod.UTF8ToString(s) : '';
    mod._free(lp);
    mod._free(p);
    return out;
  };

  const lrc74ok = '[00:00.00]一二\n[00:02.00]三四\n';
  check(info74(lrc74ok).includes('挂上 4 字') &&
        !info74(lrc74ok).includes('没有音头可挂'),
    '字与音头够用时报「挂上 4 字」，不提丢字', info74(lrc74ok));

  const over74 = '[00:00.00]一二三四五六七八九十\n';
  const overInfo74 = info74(over74);
  check(overInfo74.includes('按 LRC 解析 10 字，挂上 8 字') &&
        overInfo74.includes('2 字没有音头可挂'),
    '十字配八个音头：解析 10 字、挂上 8 字、丢 2 字', overInfo74);

  const many74 = Array.from({ length: 12 }, (_, i) => `[00:00.00]行${i + 1}\n`).join('');
  const manyInfo74 = info74(many74);
  check(manyInfo74.includes('挂上 8 字') && manyInfo74.includes('19 字没有音头可挂'),
    '同戳 12 行只配 8 个音头：挂上与丢字分别报数', manyInfo74);
  check(manyInfo74.includes('4 行整行没挂上'),
    '并把整行消失的行数单独点出来', manyInfo74);

  const late74 = '[00:00.00]你好\n[00:30.00]后会有期\n';
  check(info74(late74).includes('1 行整行没挂上'),
    '整行落在全曲最后一个音之后：同样算整行没挂上', info74(late74));

  // 每个音符格都带一个空的 <span class='lyric'>（占位），只数有字的那些，
  // 否则「印了几个字」会被占位符放大一倍。
  const spanText74 = (html) =>
    [...html.matchAll(/<span class='lyric'>([^<]*)<\/span>/g)]
      .map((m) => m[1]).filter((t) => t !== '');
  const c74 = makeCaller(mod);
  c74.setMidi(inj74);
  c74.setLyrics(over74);
  const spansOver = spanText74(c74.call('midi2jianpu_html', -1, 1) || '');
  check(spansOver.length === 8 && spansOver.join('') === '一二三四五六七八',
    '页面只印出「挂上」的那 8 个字，与体检口径一致', JSON.stringify(spansOver));

  c74.setLyrics(lrc74ok);
  const spansOk = spanText74(c74.call('midi2jianpu_html', -1, 1) || '');
  check(spansOk.length === 4 && spansOk.join('') === '一二三四',
    '字够用时四个字一个不少', JSON.stringify(spansOk));
}

// ---- 75. v1.114.0 网页轨道下拉框按真实 MTrk 块，不再信头部的 16 位轨数 ----
// 网页过去直接读 MThd offset 10..11 生成选项，而那个数字经常是错的：合并轨
// 后忘了改头会让 4 轨的曲子只剩 2 个选项，溢出成 0 则一个选项都没有（选不了
// 任何轨，尽管 --track 3 照样能转），多写几轨又会凭空冒出几个空轨（选了就报
// 「未提取到音符」）。这里锁住 track_list 的清单与头部数字无关，并静态守住
// 网页真的走这条导出、头部计数只做兜底。
const partsPath75 = path.join(ROOT, 'tests', 'fixtures', 'parts.mid');
if (existsSync(partsPath75)) {
  const parts75 = new Uint8Array(readFileSync(partsPath75));
  const trackList75 = (bytes) => {
    const p = mod._malloc(bytes.length);
    mod.HEAPU8.set(bytes, p);
    const s = mod._midi2jianpu_track_list(p, bytes.length);
    const out = s ? mod.UTF8ToString(s) : '';
    mod._free(p);
    return JSON.parse(out);
  };
  // 只改头里那个 16 位轨数，文件里的 MTrk 块一个不动
  const withHeaderCount75 = (bytes, n) => {
    const copy = new Uint8Array(bytes);
    copy[10] = (n >> 8) & 0xff;
    copy[11] = n & 0xff;
    return copy;
  };
  const hdr75 = (bytes) => (bytes[10] << 8) | bytes[11];

  const base75 = trackList75(parts75);
  check(base75.length === 4 && base75.map((t) => t.i).join(',') === '0,1,2,3',
    'track_list 列出全部 4 条真实轨道', JSON.stringify(base75));
  check(base75[0].name === 'Conductor' && base75[1].name === 'Vocal' &&
        base75[1].notes === 15 && base75[1].inst === '长笛' &&
        base75[2].notes === 12 && base75[3].notes === 8,
    '轨名 / 音数 / 乐器与 --list-tracks 同口径', JSON.stringify(base75));

  for (const fake of [0, 2, 7]) {
    const arr = trackList75(withHeaderCount75(parts75, fake));
    check(arr.length === 4,
      `头部写着 ${fake} 轨时仍按 4 个真实块给出选项（不是 ${fake} 个）`,
      JSON.stringify(arr.map((t) => t.i)));
  }
  check(hdr75(parts75) === 4,
    '素材本身的头部轨数是对的（上面几条只改了副本）');

  // 下拉框的清单不能跟着当前选中轨走，否则选完第 2 轨就只剩一个选项
  if (mod._midi2jianpu_set_track) {
    mod._midi2jianpu_set_track(2);
    check(trackList75(parts75).length === 4,
      '选中某一轨之后 track_list 仍列出全部轨');
    mod._midi2jianpu_set_track(-1);
  }

  // 轨名是 0x03 里的自由文本：引号 / 反斜杠 / 换行 / 裸 C0 都要转义，
  // 否则 JSON.parse 抛错，整个下拉框会变成空的。
  const be32 = (n) => [(n >>> 24) & 255, (n >>> 16) & 255, (n >>> 8) & 255, n & 255];
  const vlq = (n) => { const o = []; let b = n & 0x7f; n >>= 7; while (n) { o.unshift((b | 0x80) & 0xff); b = n & 0x7f; n >>= 7; } o.push(b); return o; };
  const weird = smfWithName([0x61, 0x22, 0x62, 0x5c, 0x63, 0x0a, 0x64, 0x01, 0x65]); // a"b\c\nd<C0>e
  function smfWithName(nameBytes) {
    const body = [...vlq(0), 0xff, 0x03, ...vlq(nameBytes.length), ...nameBytes,
                  ...vlq(0), 0x90, 60, 100, ...vlq(480), 0x80, 60, 0,
                  ...vlq(0), 0xff, 0x2f, 0x00];
    const head = [0x4d, 0x54, 0x68, 0x64, ...be32(6), 0, 1, 0, 1, 0x01, 0xe0];
    return new Uint8Array([...head, 0x4d, 0x54, 0x72, 0x6b, ...be32(body.length), ...body]);
  }
  let weirdParsed = null;
  try { weirdParsed = trackList75(weird); } catch (e) { weirdParsed = null; }
  check(weirdParsed && weirdParsed.length === 1 &&
        weirdParsed[0].name === 'a"b\\c\nd\u0001e',
    '轨名里的引号 / 反斜杠 / 换行 / 裸 C0 都正确转义，JSON 仍可解析',
    weirdParsed ? JSON.stringify(weirdParsed[0].name) : 'JSON.parse 失败');

  const page75 = readFileSync(path.join(ROOT, 'docs', 'index.html'), 'utf8');
  check(page75.includes('_midi2jianpu_track_list(') &&
        page75.includes('function trackListFromWasm') &&
        page75.includes('function trackListFromHeader'),
    '网页走 track_list 生成下拉框，头部计数只作旧 wasm 的兜底');
  check(page75.includes('for (var i = 0; i < tracks.length; i++)'),
    '网页按 track_list 的条目数逐个建选项（不再用头部数字当循环上界）');
}

// ---- 76. v1.115.0 网页「自带几个歌词音节」要说清在哪条轨 ----
// 旧页面只数 normalizeMidiLyrics 的音节数，一句「此文件自带 10 个歌词音节」
// 既不说词在哪条轨，也不说另有一条歌词轨整轨被忽略；更糟的是在单轨模式下
// 选了一条没有词的轨，页面会说「此文件没有内置歌词」——文件明明有词，只是
// 不在这一轨上，用户会去找一个根本不存在的 LRC。现在这句由 C++ 的
// midiLyricHint 统一算（与 CLI 同一个 pickMidiLyricTrack 口径），网页只负责显示。
const twoLyrics76 = path.join(ROOT, 'tests', 'fixtures', 'twolyrics.mid');
if (existsSync(twoLyrics76)) {
  const two76 = new Uint8Array(readFileSync(twoLyrics76));
  const hint76 = (track) => {
    mod._midi2jianpu_set_track(track);
    const p = mod._malloc(two76.length);
    mod.HEAPU8.set(two76, p);
    const s = mod._midi2jianpu_midi_lyric_info(p, two76.length);
    const out = s ? mod.UTF8ToString(s) : '';
    mod._free(p);
    return out;
  };
  const auto76 = hint76(-1);
  check(auto76.includes('此文件自带 10 个歌词音节（轨 1）'),
    '自动模式点明词在第几轨', auto76);
  check(auto76.includes('另 1 轨的 10 个音节已忽略'),
    '另一条歌词轨被整轨忽略这件事也要说', auto76);

  const none76 = hint76(0);
  check(none76.includes('轨 0 没有歌词') && none76.includes('10 个音节在轨 1'),
    '选中没有词的轨时指出词其实在哪条轨', none76);
  check(!none76.includes('此文件没有内置歌词'),
    '选中没有词的轨时不能说「此文件没有」', none76);
  check(none76.includes('「自动」'),
    '并给出可操作的建议（把轨道选回自动）', none76);

  const other76 = hint76(2);
  check(other76.includes('轨 2 自带 10 个歌词音节') &&
        other76.includes('另 1 轨的 10 个音节不参与'),
    '选中带词的轨时注明别的轨这次不参与', other76);
  mod._midi2jianpu_set_track(-1);

  // 「歌词 N 字」曾是 0x05 事件数：karaoke.mid 的 Vocal 轨有 9 条事件
  // （@KMIDI / - / _ / / 都在里面），真正能唱的只有 5 个字。
  const kar76 = new Uint8Array(readFileSync(path.join(ROOT, 'tests', 'fixtures', 'karaoke.mid')));
  const kp76 = mod._malloc(kar76.length);
  mod.HEAPU8.set(kar76, kp76);
  const klist76 = JSON.parse(mod.UTF8ToString(mod._midi2jianpu_track_list(kp76, kar76.length)));
  mod._free(kp76);
  check(klist76.length === 3 && klist76[1].lyrics === 5,
    'track_list 的歌词字数是可唱音节数，不是 0x05 事件数（9）',
    JSON.stringify(klist76.map((t) => t.lyrics)));

  // 没有词的谱：说得出口「没有内置歌词」，而不是空串让提示栏开天窗。
  const plain76 = new Uint8Array(readFileSync(path.join(ROOT, 'tests', 'fixtures', 'parts.mid')));
  const pp76 = mod._malloc(plain76.length);
  mod.HEAPU8.set(plain76, pp76);
  const plainHint76 = mod.UTF8ToString(mod._midi2jianpu_midi_lyric_info(pp76, plain76.length));
  mod._free(pp76);
  check(plainHint76 === '此文件没有内置歌词', '无内置歌词的文件如实说明', plainHint76);

  const page76 = readFileSync(path.join(ROOT, 'docs', 'index.html'), 'utf8');
  check(page76.includes('_midi2jianpu_midi_lyric_info(') &&
        page76.includes('_midi2jianpu_midi_lyric_count('),
    '网页优先用 lyric_info 说话，只给旧 wasm 留 count 兜底');
}

// ---- 77. v1.116.0 选中没有旋律音符的轨：说清里面有什么、旋律在哪 ----
// 解析层按轨过滤后只剩这一条轨，别的轨的音符已经丢了，所以「旋律在轨 1」
// 只能靠全曲重解析说出来（只走这条出错路径）。旧代码更糟：选中一条鼓轨会报
// 「文件里只有打击乐轨」，而 drums.mid 有 32 个旋律音，只是不在那条轨上。
{
  const two77 = new Uint8Array(readFileSync(path.join(ROOT, 'tests', 'fixtures', 'twolyrics.mid')));
  const drum77 = new Uint8Array(readFileSync(path.join(ROOT, 'tests', 'fixtures', 'drums.mid')));
  const at77 = (bytes, track, fn) => {
    mod._midi2jianpu_set_track(track);
    const p = mod._malloc(bytes.length);
    mod.HEAPU8.set(bytes, p);
    const s = mod[fn](p, bytes.length, 0, 1);
    const out = s ? mod.UTF8ToString(s) : '';
    mod._free(p);
    mod._midi2jianpu_set_track(-1);
    return out;
  };

  const ly77 = at77(two77, 2, '_midi2jianpu_parse');
  check(ly77.includes('轨 2 没有旋律音符') && ly77.includes('只有 10 个歌词音节') &&
        ly77.includes('旋律在轨 1（8 音）'),
    '只有歌词的轨说清里面有什么、旋律在哪', ly77);
  check(!ly77.includes('未提取到音符'), '不再只用一句「未提取到音符」打发', ly77);

  const dr77 = at77(drum77, 3, '_midi2jianpu_parse');
  check(dr77.includes('只有 192 个鼓件音') && dr77.includes('旋律在轨 2（24 音）'),
    '网页选中鼓轨时给出鼓件音数与旋律轨', dr77);
  check(!dr77.includes('文件里只有打击乐'),
    '选的是鼓轨，不能说成「文件里只有打击乐」（全曲还有 32 个旋律音）', dr77);

  const html77 = at77(two77, 2, '_midi2jianpu_html');
  check(html77.startsWith('<p>') && html77.includes('旋律在轨 1（8 音）') &&
        html77.trimEnd().endsWith('</p>'),
    'HTML 通道的错误页是同一句话、且仍是完整的一段', html77);

  // 全曲模式（没选轨）说的话不变：这时「这个文件没有旋律」才是真的
  const all77 = at77(drum77, -1, '_midi2jianpu_parse');
  check(!all77.startsWith('错误:'), '全曲模式照常转换，不冒「没有旋律音符」', all77.slice(0, 60));

  // 下拉框：有词没音符的轨不能再标成「无音符」，否则用户根本不会去点它
  const page77 = readFileSync(path.join(ROOT, 'docs', 'index.html'), 'utf8');
  check(page77.includes("（只有歌词 ' + t.lyrics + ' 字）"),
    '下拉框把「有词没音符」的轨标成「只有歌词 N 字」');
  check(page77.includes('_midi2jianpu_track_list('),
    '这条标签读的是 track_list 的每轨歌词数，不是头部轨数');
}

// ---- 78. v1.117.0 track_list 的每轨通道布局 ----
// 一条轨混多个通道（format 0 的常态、DAW 的「主旋律+伴奏一条轨」）时，
// 下拉框里的乐器名只代表主导通道。不把通道列表带出来，用户没法解释
// 「同一条轨里为什么有两种乐器」，也不知道 --track 切不开这层结构。
{
  const trio78 = new Uint8Array(readFileSync(path.join(ROOT, 'tests', 'fixtures', 'voice_trio.mid')));
  const p78 = mod._malloc(trio78.length);
  mod.HEAPU8.set(trio78, p78);
  const tl78 = JSON.parse(mod.UTF8ToString(mod._midi2jianpu_track_list(p78, trio78.length)));
  mod._free(p78);

  check(Array.isArray(tl78[1].channels) && tl78[1].channels.length === 3,
    'track_list 带出每轨的通道数（voice_trio 的旋律轨是 3 个通道）',
    JSON.stringify(tl78.map((t) => t.channels)));
  check(JSON.stringify(tl78[1].channels) === '[0,1,2]',
    '通道号按升序给出，不重复', JSON.stringify(tl78[1].channels));
  check(Array.isArray(tl78[0].channels) && tl78[0].channels.length === 0,
    '没有音符的 conductor 轨给空数组，不是 null', JSON.stringify(tl78[0]));

  // 纯鼓轨的通道表里是通道 9 —— 鼓件音也算一次 note-on，
  // 否则这条轨在诊断里会变成「没有任何通道」。
  const drum78 = new Uint8Array(readFileSync(path.join(ROOT, 'tests', 'fixtures', 'drums.mid')));
  const pd78 = mod._malloc(drum78.length);
  mod.HEAPU8.set(drum78, pd78);
  const dl78 = JSON.parse(mod.UTF8ToString(mod._midi2jianpu_track_list(pd78, drum78.length)));
  mod._free(pd78);
  check(JSON.stringify(dl78[3].channels) === '[9]',
    '鼓轨的通道是 9（鼓件音同样计入通道布局）', JSON.stringify(dl78[3].channels));

  // 老字段一个都不能少：网页的当前版本仍读 notes / drum / lyrics / inst
  const ok78 = ['i', 'name', 'notes', 'drum', 'lyrics', 'inst', 'channels']
    .every((k) => k in dl78[1]);
  check(ok78, 'track_list 的既有字段全部保留，只新增 channels', JSON.stringify(dl78[1]));

  const page78 = readFileSync(path.join(ROOT, 'docs', 'index.html'), 'utf8');
  check(page78.includes("' · 通道 ' + t.channels.join(',')"),
    '下拉框对多通道轨补一句「通道 0,1」');
}

// ---- 79. v1.118.0 track_list 的逐通道乐器名 ----
// 只有通道号还不够：同一轨 ch0 钢琴、ch1 小提琴时，下拉框仍写着笼统的
// 「乐器: 钢琴」。新字段 insts 把每条通道在它首音时刻的 GM 名带出来。
{
  const vlq79 = (n) => {
    const o = []; let b = n & 0x7f; n >>= 7;
    while (n) { o.unshift((b | 0x80) & 0xff); b = n & 0x7f; n >>= 7; }
    o.push(b); return o;
  };
  const be32 = (n) => [(n >>> 24) & 255, (n >>> 16) & 255, (n >>> 8) & 255, n & 255];
  const body79 = [
    ...vlq79(0), 0xff, 0x03, 3, 0x44, 0x75, 0x6f,   // "Duo"
    ...vlq79(0), 0xc0, 0,                           // 通道 0 钢琴
    ...vlq79(0), 0xc1, 40,                          // 通道 1 小提琴 (GM 40)
    ...vlq79(0), 0x90, 60, 90, ...vlq79(480), 0x80, 60, 0,
    ...vlq79(0), 0x91, 67, 90, ...vlq79(480), 0x81, 67, 0,
    ...vlq79(0), 0xff, 0x2f, 0,
  ];
  const head79 = [0x4d, 0x54, 0x68, 0x64, ...be32(6), 0, 1, 0, 1, 0x01, 0xe0];
  const dual79 = new Uint8Array([...head79, 0x4d, 0x54, 0x72, 0x6b,
    ...be32(body79.length), ...body79]);
  const p79 = mod._malloc(dual79.length);
  mod.HEAPU8.set(dual79, p79);
  const tl79 = JSON.parse(mod.UTF8ToString(mod._midi2jianpu_track_list(p79, dual79.length)));
  mod._free(p79);
  check(tl79.length === 1 && Array.isArray(tl79[0].insts) && tl79[0].insts.length === 2,
    'track_list 带出同轨每通道的乐器名', JSON.stringify(tl79[0]));
  check(JSON.stringify(tl79[0].insts) === '[{"c":0,"n":"钢琴"},{"c":1,"n":"小提琴"}]',
    '逐通道乐器按通道号升序、名字可读', JSON.stringify(tl79[0].insts));
  check(['i', 'name', 'notes', 'drum', 'lyrics', 'inst', 'channels', 'insts']
    .every((k) => k in tl79[0]),
  'insts 作为新字段加入，既有字段一个不少', JSON.stringify(tl79[0]));

  // 没有 0xC0 的多通道轨：insts 是空数组，不能凭空编一个乐器出来。
  const trio79 = new Uint8Array(readFileSync(path.join(FIXTURES, 'voice_trio.mid')));
  const pt79 = mod._malloc(trio79.length);
  mod.HEAPU8.set(trio79, pt79);
  const t79 = JSON.parse(mod.UTF8ToString(mod._midi2jianpu_track_list(pt79, trio79.length)));
  mod._free(pt79);
  check(Array.isArray(t79[1].insts) && t79[1].insts.length === 0,
    '没有音色声明的多通道轨 insts 为空数组，不是 null', JSON.stringify(t79[1]));

  const page79 = readFileSync(path.join(ROOT, 'docs', 'index.html'), 'utf8');
  check(page79.includes("t.insts.map(function (x) { return x.c + '=' + x.n; })"),
    '网页下拉框把每个通道的乐器名点出来');
}

// ---- 80. v1.119.0 track_list 的鼓件名 ----
// 「跳过 192 鼓件音」回答不了「丢的是底鼓还是镲片」。新字段 druminsts
// 由诊断层统一生成，命令行与网页不会再各写一套鼓名表。
{
  const drums80 = new Uint8Array(readFileSync(path.join(FIXTURES, 'drums.mid')));
  const p80 = mod._malloc(drums80.length);
  mod.HEAPU8.set(drums80, p80);
  const tl80 = JSON.parse(mod.UTF8ToString(mod._midi2jianpu_track_list(p80, drums80.length)));
  mod._free(p80);
  const dr80 = tl80[3];
  check(dr80.drum === 192 && dr80.druminsts === '开镲 64, 闭镲 64, 军鼓 32, 底鼓 32',
    'track_list 把鼓件按中文名与次数带出来', JSON.stringify(dr80));
  check(['i', 'name', 'notes', 'drum', 'lyrics', 'inst', 'channels', 'insts', 'druminsts']
    .every((k) => k in dr80),
  'druminsts 作为新字段加入，既有字段一个不少', JSON.stringify(dr80));
  check(tl80[1].druminsts === '' && tl80[2].druminsts === '',
    '非鼓轨的 druminsts 是空串，不会凭空多一句', JSON.stringify(tl80[1]));

  const page80 = readFileSync(path.join(ROOT, 'docs', 'index.html'), 'utf8');
  check(page80.includes("if (t.druminsts) label += '：' + t.druminsts;"),
    '网页下拉框把鼓件清单接在「跳过 N 鼓件音」后面');
}

// ---- 81. v1.120.0 track_list 的鼓件清单列全 ----
// 默认 4 项上限只给空轨提示；--list-tracks 与 track_list 是完整诊断。
// sysex_ch9_normal.mid 有 5 种鼓件，旧版报「高音邦戈 1 等」，第 5 种被吞。
{
  const five = new Uint8Array(readFileSync(path.join(FIXTURES, 'sysex_ch9_normal.mid')));
  const pf = mod._malloc(five.length);
  mod.HEAPU8.set(five, pf);
  const tlf = JSON.parse(mod.UTF8ToString(mod._midi2jianpu_track_list(pf, five.length)));
  mod._free(pf);
  const df = tlf[1];
  check(df.drum === 8 && df.druminsts.includes('高音阿哥哥 1') &&
    !df.druminsts.includes('等'),
  'track_list 列全五种鼓件，末尾没有「等」', JSON.stringify(df));
  check(tlf.every((t) => !t.druminsts.includes('等')),
    '整份文件的鼓件清单都没有截断标记', JSON.stringify(tlf));
}

// ---- 82. v1.121.0 track_list 写明「未指定音色」 ----
// programAt 返回 -1 的含义是「文件没写 0xC0」，不是「读不出来」；GM 规范下
// 这种通道就是 0 号音色。inst 现在把默认值写明，program 的语义不变，逐通道
// insts 仍是空数组——那里回答的是另一个问题：各通道各自声明了什么。
{
  const np = new Uint8Array(readFileSync(path.join(FIXTURES, 'bank_no_program.mid')));
  const pn = mod._malloc(np.length);
  mod.HEAPU8.set(np, pn);
  const tln = JSON.parse(mod.UTF8ToString(mod._midi2jianpu_track_list(pn, np.length)));
  mod._free(pn);
  check(tln[1].inst === '未指定（GM 默认为钢琴）' &&
    tln[2].inst === '未指定（GM 默认为钢琴）',
  '没有 0xC0 的旋律轨在 inst 里写明「未指定」与 GM 默认音色',
  JSON.stringify(tln.map((t) => t.inst)));
  check(tln[0].inst === '',
    '没有音符的指挥轨 inst 仍是空串，不冒充钢琴', JSON.stringify(tln[0]));
  check(Array.isArray(tln[1].insts) && tln[1].insts.length === 0,
    '逐通道 insts 仍为空数组：它答的是「各通道声明了什么」',
    JSON.stringify(tln[1].insts));

  // 每条轨都写了音色的文件一个字段都不变。
  const pm = new Uint8Array(readFileSync(path.join(FIXTURES, 'parts.mid')));
  const ppm = mod._malloc(pm.length);
  mod.HEAPU8.set(pm, ppm);
  const tlp = JSON.parse(mod.UTF8ToString(mod._midi2jianpu_track_list(ppm, pm.length)));
  mod._free(ppm);
  check(tlp[1].inst === '长笛' && tlp[2].inst === '钢琴' &&
    !tlp.some((t) => t.inst.includes('未指定')),
  '都声明了音色的文件照旧只报乐器名', JSON.stringify(tlp.map((t) => t.inst)));
}

// ---- 83. v1.122.0 track_list 的轨名：控制字符只以 \u00XX 出现 ----
// 网页下拉框用 innerHTML 拼选项，轨名里的 ESC 若原样进 JSON → 原样进页面。
// jsonEscape 早就在转 C0，这里把它钉死：JSON 文本里一个裸 C0 / DEL 都不许有，
// 而解析回来又要与文件里的字节一字不差（转义不能丢信息）。
// 注意 40 字符上限只服务命令行那张表，网页下拉框要完整名字。
{
  const hp = new Uint8Array(readFileSync(path.join(FIXTURES, 'hostile_name.mid')));
  const ph = mod._malloc(hp.length);
  mod.HEAPU8.set(hp, ph);
  const rawJson = mod.UTF8ToString(mod._midi2jianpu_track_list(ph, hp.length));
  mod._free(ph);
  check(!/[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]/.test(rawJson),
    'track_list 的 JSON 文本里没有裸控制字符',
    JSON.stringify(rawJson.slice(0, 160)));
  const tlh = JSON.parse(rawJson);
  check(tlh[1].name === 'Line\nBreak "Q" \\ \u001b[31mRED\u001b[0m \u0001END\u007f',
    '控制字符在 JSON 里往返一圈回到原样（转义不丢信息）',
    JSON.stringify(tlh[1].name));
  check(rawJson.includes('\\u001b') && rawJson.includes('\\u0001') &&
    rawJson.includes('\\u007f'),
  'JSON 文本里是 \\u00XX 而不是裸字节（下拉框 textContent 也不会渲染出控制符）',
  JSON.stringify(rawJson.slice(0, 160)));
  check(tlh[2].name === '长'.repeat(80),
    'JSON 不做 40 字符截断：网页下拉框要完整名字，上限只服务命令行那张表',
    String(tlh[2].name.length));
}

// ---- 84. v1.124.0 轨号超过 255：音符归属不串轨 ----
// active 表把 (通道, 音高, 轨号) 压成一个 64 位 key，轨号原本只占低 8 位：
// 第 256 条轨的同一个 (通道, 音高) 会和轨 0 撞成一个 key —— 网页的段落
// 下拉框会显示轨 0「2 音」、轨 256「0 音」，选中轨 256 后谱面一片空白。
// 这里现场拼一份 257 轨的 SMF：每轨同通道、同 tick、同音高，最容易撞。
{
  const tracks = 257;
  const body = new Uint8Array([
    0x00, 0x90, 0x3c, 0x64,   // tick 0 按下 C4
    0x83, 0x60, 0x80, 0x3c, 0x00,  // tick 480 抬手
    0x00, 0xff, 0x2f, 0x00,   // EOT
  ]);
  const chunk = new Uint8Array(8 + body.length);
  chunk.set([0x4d, 0x54, 0x72, 0x6b], 0);            // "MTrk"
  new DataView(chunk.buffer).setUint32(4, body.length);
  chunk.set(body, 8);

  const many = new Uint8Array(14 + tracks * chunk.length);
  many.set([0x4d, 0x54, 0x68, 0x64], 0);             // "MThd"
  const hv = new DataView(many.buffer, 4, 10);
  hv.setUint32(0, 6);
  hv.setUint16(4, 1);            // format 1
  hv.setUint16(6, tracks);       // 头里的 16 位轨数
  hv.setUint16(8, 480);          // division
  for (let i = 0; i < tracks; i++) many.set(chunk, 14 + i * chunk.length);

  const pmany = mod._malloc(many.length);
  mod.HEAPU8.set(many, pmany);
  const tlm = JSON.parse(mod.UTF8ToString(mod._midi2jianpu_track_list(pmany, many.length)));
  mod._free(pmany);
  check(tlm.length === tracks, 'track_list 给出 257 条（下拉框不会少一段）',
    String(tlm.length));
  const badM = tlm.filter((t) => t.notes !== 1);
  check(badM.length === 0,
    '257 条轨每轨 1 个音（旧实现轨 0 报 2、轨 256 报 0）',
    JSON.stringify(badM.slice(0, 3).map((t) => t.notes)));

  mod._midi2jianpu_set_track(tracks - 1);
  const callerM = makeCaller(mod);
  callerM.setMidi(many);
  const txtM = callerM.call('midi2jianpu_parse', -1, 1) || '';
  // WASM 的 parse 不带 CLI 那行「共提取 N 个音符」，谱面自己写着音数
  check(/声部：轨 256/.test(txtM) && /\(1 音符\)/.test(txtM),
    '选中第 256 段时网页谱面拿到自己那 1 个音（旧实现是空的）',
    txtM.slice(0, 120));
  mod._midi2jianpu_set_track(-1);
}

// ---- 85. v1.125.0 巨额 delta 不翻负 ----
// 每条事件前都写满 4 字节 VLQ 上限（0x0FFFFFFF）：18 个 delta 累到 4.83e9，
// 32 位 long 会在第 9 个 delta 之后翻负。负 tick 躲过「tick >= cap」这道
// 单侧闸，网页版曾把 4 个幽灵音（tick ≈ -1.88e9）画进谱面，两段之间
// 5 亿 tick 的空档被铺成一整行 670 万字符的横线。
{
  const bodyU = [];
  for (let i = 0; i < 9; i++) {
    bodyU.push(0xff, 0xff, 0xff, 0x7f, 0x90, 60 + i, 0x64);   // delta, note-on
    bodyU.push(0xff, 0xff, 0xff, 0x7f, 0x80, 60 + i, 0x00);   // delta, note-off
  }
  bodyU.push(0x00, 0xff, 0x2f, 0x00);                          // EOT
  const hb = new Uint8Array(bodyU);
  const hchunk = new Uint8Array(8 + hb.length);
  hchunk.set([0x4d, 0x54, 0x72, 0x6b], 0);                     // "MTrk"
  new DataView(hchunk.buffer).setUint32(4, hb.length);
  hchunk.set(hb, 8);

  const huge = new Uint8Array(14 + hchunk.length);
  huge.set([0x4d, 0x54, 0x68, 0x64], 0);                       // "MThd"
  const hh = new DataView(huge.buffer, 4, 10);
  hh.setUint32(0, 6);
  hh.setUint16(4, 1);            // format 1
  hh.setUint16(6, 1);            // 1 条轨
  hh.setUint16(8, 480);          // division
  huge.set(hchunk, 14);

  const phHuge = mod._malloc(huge.length);
  mod.HEAPU8.set(huge, phHuge);
  const tlHuge = JSON.parse(
    mod.UTF8ToString(mod._midi2jianpu_track_list(phHuge, huge.length)));
  mod._free(phHuge);
  check(tlHuge.length === 1 && tlHuge[0].notes === 0,
    '9 个巨额 delta 的音一个都不进谱（旧实现漏出 4 个 tick 为负的幽灵音）',
    JSON.stringify(tlHuge.map((t) => t.notes)));

  const cHuge = makeCaller(mod);
  cHuge.setMidi(huge);
  const txtHuge = cHuge.call('midi2jianpu_parse', -1, 1) || '';
  // 网页版没有音符时返回那句短提示（CLI 走 stderr，网页走返回值）
  check(txtHuge.length < 100 && /未提取到音符/.test(txtHuge),
    '网页谱面只回一句「未提取到音符」，不吐那行 670 万字符的横线',
    txtHuge.slice(0, 80));
}

// ---- 86. v1.126.0 VLQ 最多 4 字节 ----
// SMF 规范：VLQ 上限 4 字节。旧实现 while 一路吃、uint32 回绕：5 字节
// VLQ 的真实 35 位值 0x1_0000_0005 被悄悄当成 5，delta 回绕成小值就
// 凭空造音符（网页谱面多出一个音、零诊断）。现在 4 字节封顶，越界
// 按损坏事件计数并停在本轨。
{
  const bodyU = [
    0x00, 0x90, 0x3c, 0x64,                       // note-on 60
    0x83, 0x60, 0x80, 0x3c, 0x00,                 // delta 480, note-off
    0x90, 0x80, 0x80, 0x80, 0x05,                 // 5 字节 VLQ（非法，回绕成 5）
    0x90, 0x3e, 0x64,                             // 旧实现凭空多出的音
    0x00, 0xff, 0x2f, 0x00,                       // EOT
  ];
  const hb = new Uint8Array(bodyU);
  const chunk = new Uint8Array(8 + hb.length);
  chunk.set([0x4d, 0x54, 0x72, 0x6b], 0);         // "MTrk"
  new DataView(chunk.buffer).setUint32(4, hb.length);
  chunk.set(hb, 8);

  const mid = new Uint8Array(14 + chunk.length);
  mid.set([0x4d, 0x54, 0x68, 0x64], 0);           // "MThd"
  const mh = new DataView(mid.buffer, 4, 10);
  mh.setUint32(0, 6);
  mh.setUint16(4, 1);                             // format 1
  mh.setUint16(6, 1);                             // 1 条轨
  mh.setUint16(8, 480);                           // division
  mid.set(chunk, 14);

  const ph = mod._malloc(mid.length);
  mod.HEAPU8.set(mid, ph);
  const tl = JSON.parse(
    mod.UTF8ToString(mod._midi2jianpu_track_list(ph, mid.length)));
  mod._free(ph);
  check(tl.length === 1 && tl[0].notes === 1,
    '非法 5 字节 VLQ 之后的假音符不再进谱（旧实现 2 个音）',
    JSON.stringify(tl.map((t) => t.notes)));

  const c = makeCaller(mod);
  c.setMidi(mid);
  const txt = c.call('midi2jianpu_parse', -1, 1) || '';
  check(/\(1 音符\)/.test(txt) && !/\(2 音符\)/.test(txt),
    '网页谱面只有真实的那 1 个音',
    txt.slice(0, 60));
}

// ---- 87. v1.142.0 format 2 的轨道诊断要按整份文件解析 ----
// 谱面默认只转 format 2 的第 0 段，这是设计；但网页下拉框与歌词体检说的是
// 「整份文件有什么」。旧实现两条诊断都走默认解析，SongB 的轨名 / 音符 /
// 歌词在解析层就被丢掉，页面于是显示空轨并说「此文件没有内置歌词」。
{
  const trackList = (name) => {
    const bytes = new Uint8Array(readFileSync(path.join(FIXTURES, name)));
    const p = mod._malloc(bytes.length);
    mod.HEAPU8.set(bytes, p);
    const s = mod._midi2jianpu_track_list(p, bytes.length);
    const out = s ? mod.UTF8ToString(s) : '';
    mod._free(p);
    return JSON.parse(out);
  };
  const lyricHint = (name, track) => {
    const bytes = new Uint8Array(readFileSync(path.join(FIXTURES, name)));
    mod._midi2jianpu_set_track(track);
    const p = mod._malloc(bytes.length);
    mod.HEAPU8.set(bytes, p);
    const s = mod._midi2jianpu_midi_lyric_info(p, bytes.length);
    const out = s ? mod.UTF8ToString(s) : '';
    mod._free(p);
    return out;
  };

  const a = trackList('format2.mid');
  check(a.length === 2 && a[0].name === 'SongA' && a[1].name === 'SongB',
    'format2.mid 的下拉框列出两段真实轨名（旧版 SongB 是空名）',
    JSON.stringify(a.map((t) => t.name)));
  check(a[0].notes === 16 && a[1].notes === 8,
    '两段各自的音符数都保留（旧版第二段 0 音）',
    JSON.stringify(a.map((t) => t.notes)));

  const b = trackList('track_fmt2.mid');
  check(b.length === 2 && b[0].name === 'SongA' && b[1].name === 'SongB',
    '带内置歌词的 format 2 也能列出两段轨名',
    JSON.stringify(b.map((t) => t.name)));
  check(b[0].notes === 4 && b[1].notes === 4 && b[1].lyrics === 8,
    '第二段的 4 音与 8 个歌词音节都进诊断（与 CLI --list-tracks 一致）',
    JSON.stringify(b.map((t) => [t.notes, t.lyrics])));

  const autoHint = lyricHint('track_fmt2.mid', -1);
  check(autoHint.includes('此文件自带 8 个歌词音节（轨 1）'),
    '自动模式指出词在第二段（旧版只说「没有内置歌词」）', autoHint);
  const noneHint = lyricHint('track_fmt2.mid', 0);
  check(noneHint.includes('8 个音节在轨 1') &&
        !noneHint.includes('此文件没有内置歌词'),
    '选了没词的轨时指路到轨 1，不说「此文件没有」', noneHint);
  const oneHint = lyricHint('track_fmt2.mid', 1);
  check(oneHint.includes('轨 1 自带 8 个歌词音节'),
    '选中带词的第二段时如实报 8 字', oneHint);
  const twoHint = lyricHint('track_fmt2.mid', 2);
  check(twoHint.includes('轨 2 没有歌词') && twoHint.includes('8 个音节在轨 1'),
    '越界轨也不会把第二段的词说成不存在', twoHint);

  mod._midi2jianpu_set_track(-1);
}

// ---- 88. v1.143.0 网页歌词体检跟随「力度与演奏法」开关 ----
// overfull.mid 的 8 个音里有 1 个倚音。开启力度时倚音不占词位，8 字只挂 7；
// 关闭力度后 ExpressionMap 整体停用，倚音按普通音头参与铺排，8 字全挂上。
// 旧版 lyric_info() 漏了 applyRenderOptions()，两种情况都答「挂上 7 字」，
// 关闭力度时就和同一份 HTML 的 8 字自相矛盾。
{
  const midi = new Uint8Array(readFileSync(path.join(FIXTURES, 'overfull.mid')));
  const lrc = '[00:00.00]一二三四五六七八\n';
  const run = (dyn) => {
    mod._midi2jianpu_set_use_dynamics(dyn);
    const p = mod._malloc(midi.length);
    mod.HEAPU8.set(midi, p);
    const n = mod.lengthBytesUTF8(lrc) + 1;
    const lp = mod._malloc(n);
    mod.stringToUTF8(lrc, lp, n);
    mod._midi2jianpu_set_lyrics(lp);
    const info = mod.UTF8ToString(mod._midi2jianpu_lyric_info(p, midi.length, lp));
    const html = mod.UTF8ToString(mod._midi2jianpu_html(p, midi.length));
    mod._free(lp);
    mod._free(p);
    const words = [...html.matchAll(/class='lyric'>([^<]*)</g)]
      .map((m) => m[1]).filter((w) => w.trim());
    return { info, words };
  };

  const on = run(1);
  check(on.info.includes('挂上 7 字') && on.info.includes('1 字没有音头可挂'),
    '开启力度时体检报 7 字并说明倚音不占词位', on.info);
  check(on.words.length === 7 && on.words.join('') === '一二三四五六七',
    '开启力度时谱面确实只印前 7 字', on.words.join(''));

  const off = run(0);
  check(off.info.includes('挂上 8 字') && !off.info.includes('没有音头可挂'),
    '关闭力度后体检报 8 字，不再误报丢字', off.info);
  check(off.words.length === 8 && off.words.join('') === '一二三四五六七八',
    '关闭力度后谱面 8 字全印，体检与谱面一致', off.words.join(''));

  const infoCount = (s) => Number((s.match(/挂上 (\d+) 字/) || [])[1]);
  check(infoCount(on.info) === on.words.length &&
        infoCount(off.info) === off.words.length,
    '两种开关下体检报的「挂上 N 字」都等于谱面实际字数',
    `${on.info} / ${on.words.length}；${off.info} / ${off.words.length}`);

  mod._midi2jianpu_set_use_dynamics(1);
}

// ---- 89. v1.144.0 倚音不再吞掉它前面的休止列 ----
// HTML 的倚音分支原来直接 continue，把「倚音前面那段空拍」一起跳过。
// parts.mid 的 2 拍弱起因此只画出 1 拍（少了开头的 0 列）；网页端与
// 命令行、MusicXML 的谱面结构就此分叉。
{
  for (const [name, barIndex, wantCols] of
       [['parts.mid', 0, 2], ['overfull.mid', 2, 3]]) {
    const midi = new Uint8Array(readFileSync(path.join(FIXTURES, name)));
    const p = mod._malloc(midi.length);
    mod.HEAPU8.set(midi, p);
    const html = mod.UTF8ToString(mod._midi2jianpu_html(p, midi.length));
    mod._free(p);
    const bars = html.split("<span class='measure'>").slice(1);
    const bar = bars[barIndex] || '';
    const cols = (bar.match(/<span class='note /g) || []).length;
    check(cols === wantCols,
      `${name} 第 ${barIndex + 1} 小节 ${wantCols} 列（旧版少一列）`,
      `实际 ${cols} 列`);
    check(bar.includes("<span class='num'>0</span>"),
      `${name} 第 ${barIndex + 1} 小节的空拍补成了 0 列`, bar.slice(0, 80));
    if (bar.includes("class='grace'")) {
      const r0 = bar.indexOf("<span class='num'>0</span>");
      const g0 = bar.indexOf("class='grace'");
      check(r0 >= 0 && r0 < g0, `${name} 的 0 列排在倚音之前`,
        `rest=${r0} grace=${g0}`);
    }
  }
}

// ---- 90. v1.145.0 --no-dynamics 不再抹掉 MusicXML 的速度 ----
// 速度记号原先整块长在 emitDirections 里，而那个 lambda 的第一句就是
// `if (!expr.enabled()) return;`。网页端勾掉「力度与演奏法」后，MusicXML
// 里的 <metronome> / <sound tempo> 跟着一起消失（midtempo.mid 4 -> 1），
// 导进 MuseScore 一律按 120 播到底。速度是回放速度，不是演奏法。
{
  const temposOf = (frame, name) => {
    const midi = new Uint8Array(readFileSync(path.join(FIXTURES, name)));
    const p = mod._malloc(midi.length);
    mod.HEAPU8.set(midi, p);
    const xml = mod.UTF8ToString(
      mod._midi2jianpu_musicxml(p, midi.length, frame[0], frame[1])) || '';
    mod._free(p);
    return [...xml.matchAll(/<sound tempo="(\d+)"\/>/g)].map((m) => m[1]).join(',');
  };
  for (const [name, want] of
       [['midtempo.mid', '120,90,60,120'], ['tempo_ramp.mid', '']]) {
    mod._midi2jianpu_set_use_dynamics(1);
    const on = temposOf([-1, 1], name);
    mod._midi2jianpu_set_use_dynamics(0);
    const off = temposOf([-1, 1], name);
    mod._midi2jianpu_set_use_dynamics(1);
    check(on === off, `${name} 网页关掉「力度与演奏法」后速度序列一字不差`,
      `${on} vs ${off}`);
    if (want) {
      check(off === want,
        `${name} 的 120→90→60→120 全在（旧版只剩 120）`, off);
    } else {
      check(off.split(',').length === 6,
        `${name} 六条速度全在（旧版 1 条）`, off);
    }
  }
}

// ---- 91. v1.146.0 MusicXML 短轨尾节补齐（各 part 等长） ----
// score-partwise 是矩形谱：各 <part> 的 <measure> 按「在自己 part 里的第几个」
// 对齐，小节数必须相同。渲染循环按片段推进，某条轨提前没音了它的 part 就停在
// 那儿（sample.mid 的 Pad 只有 3 小节，Vocal / Piano 有 4 个）。网页端与 CLI
// 共用同一份 writer，这里验证导出结果确实补齐了、而且补的是整小节休止。
{
  const partMeasures = (xml) => {
    const out = {};
    for (const m of xml.matchAll(/<part id="([^"]+)">([\s\S]*?)<\/part>/g)) {
      out[m[1]] = [...m[2].matchAll(/<measure number="([^"]+)"/g)].map((x) => x[1]);
    }
    return out;
  };
  const xmlOf = (name) => {
    const midi = new Uint8Array(readFileSync(path.join(FIXTURES, name)));
    const p = mod._malloc(midi.length);
    mod.HEAPU8.set(midi, p);
    const xml = mod.UTF8ToString(
      mod._midi2jianpu_musicxml(p, midi.length, -1, 1)) || '';
    mod._free(p);
    return xml;
  };
  for (const [name, want] of [['sample.mid', 4], ['pedal_channels.mid', 3]]) {
    const xml = xmlOf(name);
    const pm = partMeasures(xml);
    const ids = Object.keys(pm);
    check(ids.length >= 2, `${name} 至少两个 part`, ids.join(','));
    const lens = ids.map((k) => pm[k].length);
    check(lens.every((n) => n === want),
      `${name} 网页导出的各 part 小节数都等于 ${want}（旧版长短不一）`,
      lens.join(','));
  }
  // 短 part 的尾节必须是整小节休止
  const xml = xmlOf('sample.mid');
  const p3 = (xml.match(/<part id="P3">([\s\S]*?)<\/part>/) || ['', ''])[1];
  const ms = [...p3.matchAll(/<measure number="[^"]+"[^>]*>[\s\S]*?<\/measure>/g)];
  const tail = ms.length ? ms[ms.length - 1][0] : '';
  check(tail.includes('<rest measure="yes"/>') && !tail.includes('<pitch>'),
    'sample.mid 的 Pad 尾节是整小节休止', tail.slice(0, 120));
}

// ---- 92. v1.156.0 网页选调不再被页眉记到 MIDI 文件头上 ----
// 网页版的调号下拉框走 keyPitch >= 0，与 CLI 的 --key 是同一件事。页眉原来
// 只看「文件里有没有 0x59 调号事件」，于是用户自己选了 D 调，页眉却仍旧印
// 「MIDI调号」——把人的选择说成文件自带的。transpose_key.mid 里明写着 1=Bb，
// 正好用来分辨这两种情况。
{
  const midiKey = new Uint8Array(readFileSync(path.join(FIXTURES, 'transpose_key.mid')));
  const metaOf = (keyPitch, isMajor) => {
    const p = mod._malloc(midiKey.length);
    mod.HEAPU8.set(midiKey, p);
    const html = mod.UTF8ToString(
      mod._midi2jianpu_html(p, midiKey.length, keyPitch, isMajor)) || '';
    mod._free(p);
    const m = html.match(/<div class="meta">([\s\S]*?)<\/div>/);
    return m ? m[1].replace(/<[^>]+>/g, '') : '';
  };

  const auto = metaOf(-1, 1);
  check(auto.includes('MIDI调号') && !auto.includes('手动指定'),
    '自动定调时页眉照旧写 MIDI调号', auto);

  const picked = metaOf(2, 1);        // 用户选 D
  check(picked.includes('手动指定 D'), '网页选调后页眉写「手动指定 D」', picked);
  check(!picked.includes('MIDI调号'),
    '网页选调后页眉不再谎称 MIDI调号', picked);
  check(picked.includes('1=D'),
    '选 D 之后谱面确实是 1=D（页眉与调性一致）', picked);
}

// ---- 93. v1.157.0 四分三连音在网页版也成组标注，增强 LRC 不再吞整句 ----
// 网页与 CLI 共用 notation.cpp 的三连音判据：只认 1/3 体系时，四分三连音
// （每个 2/3 拍）会被当成附点八分画出去，一小节四拍变 4.25 拍。
{
  const tq = new Uint8Array(readFileSync(path.join(FIXTURES, 'triplet_quarter.mid')));
  const p = mod._malloc(tq.length);
  mod.HEAPU8.set(tq, p);
  const html = mod.UTF8ToString(mod._midi2jianpu_html(p, tq.length, -1, 1)) || '';
  const xml = mod.UTF8ToString(mod._midi2jianpu_musicxml(p, tq.length, -1, 1)) || '';

  // 歌词：未闭合的 `<数字…:` 只丢标签本体。旧版把整行剩余的字全丢掉，
  // 网页提示栏还会显示「未识别出歌词」。
  const lrc = '[00:01.00]<3 心:跳一下';
  const n = mod.lengthBytesUTF8(lrc) + 1;
  const lp = mod._malloc(n);
  mod.stringToUTF8(lrc, lp, n);
  const info = mod.UTF8ToString(mod._midi2jianpu_lyric_info(p, tq.length, lp)) || '';
  mod._free(lp);
  mod._free(p);

  check(html.includes('trip'), '四分三连音在网页谱面上成组标注（trip）',
    html.slice(0, 160));
  check(xml.includes('<time-modification><actual-notes>3</actual-notes>'),
    '网页导出的 MusicXML 带 3:2 time-modification', xml.slice(0, 160));
  check(xml.includes('<type>quarter</type>'),
    '四分三连音的 <type> 是 quarter（不是 eighth）');
  // `<3 心:跳一下` 共 8 个字（含字面量 `<3`）。旧版在这里直接把整行丢掉，
  // 网页提示栏显示「未识别出歌词行」。
  const parsed = (info.match(/，?按 (?:LRC|JSON) 解析 (\d+) 字/) || [])[1];
  check(parsed && Number(parsed) >= 7,
    '未闭合增强标签不再吞掉后面的正文（解析出 ' + parsed + ' 字）', info);
}

// ---- 94. v1.158.0 网页版也不再把 3.708 拍的休止画成 4 拍 ----
// 网页与 CLI 共用 html_writer.cpp 的休止拆列：按最近档上取整时，1780 tick
// 的休止会画成一个 4 拍的 0 列，这一小节读出 4.25 拍。修好之后拆成
// 3.5 + 0.5 + 0.125 三列，余下不足 1/8 拍的部分留成看不见的洞。
{
  const og = new Uint8Array(readFileSync(path.join(FIXTURES, 'offgrid_rest.mid')));
  const p = mod._malloc(og.length);
  mod.HEAPU8.set(og, p);
  const html = mod.UTF8ToString(mod._midi2jianpu_html(p, og.length, -1, 1)) || '';
  const xml = mod.UTF8ToString(mod._midi2jianpu_musicxml(p, og.length, -1, 1)) || '';
  mod._free(p);

  check((html.match(/class='note whole'/g) || []).length === 1,
    '网页谱面只剩第 1 小节那一个整小节休止列（第 2 小节不再画 4 拍）',
    html.slice(0, 200));
  check(html.includes('thirtysecond'),
    '休止拆到三十二分（0.125 拍）那一列，而不是整小节一列', html.slice(0, 200));
  check(xml.includes('<duration>1780</duration>'),
    '网页导出的 MusicXML 仍写真实时值 1780', xml.slice(0, 200));
}


// ---- 95. v1.164.0 网页移调：与 CLI --transpose 同一层换算、同一句措辞 ----
// 网页过去完全没有移调能力：MIDI 定在 Bb、歌手只能唱到 F，用户只能回 DAW 里
// 重新导出。这里走的是 CLI 的 transposeData()，位置也一样（解析之后、量化与
// 定调之前），所以谱面音高、调号、页眉措辞、试听音高四者不会各说各话。
{
  const tk = readFixture('transpose_key.mid');   // 文件自带 1=Bb，第 5 小节转 1=D
  const tMeta = (semis, keyPitch, isMajor) => {
    const p = mod._malloc(tk.length);
    mod.HEAPU8.set(tk, p);
    mod._midi2jianpu_set_transpose(semis);
    const html = mod.UTF8ToString(mod._midi2jianpu_html(
      p, tk.length, keyPitch === undefined ? -1 : keyPitch,
      isMajor === undefined ? 1 : isMajor)) || '';
    mod._free(p);
    const m = html.match(/<div class="meta">([\s\S]*?)<\/div>/);
    return { meta: m ? m[1].replace(/<[^>]+>/g, '') : '', html };
  };
  const tXml = (semis) => {
    const p = mod._malloc(tk.length);
    mod.HEAPU8.set(tk, p);
    mod._midi2jianpu_set_transpose(semis);
    const x = mod.UTF8ToString(mod._midi2jianpu_musicxml(p, tk.length, -1, 1)) || '';
    mod._free(p);
    return x;
  };
  const fifthsOf = (x) => [...x.matchAll(/<fifths>(-?\d+)<\/fifths>/g)].map((m) => m[1]);

  check(typeof mod._midi2jianpu_set_transpose === 'function' &&
        typeof mod._midi2jianpu_transpose_folded === 'function',
    '网页移调的两个导出都在（否则下拉框点了没反应）');

  const plain = tMeta(0);
  const up2 = tMeta(2);
  check(up2.meta.includes('1=C') && plain.meta.includes('1=Bb'),
    '移调 +2 后页眉从 1=Bb 变 1=C（调号一起平移）', up2.meta);
  check(up2.meta.includes('MIDI调号，已随移调平移'),
    '移调后页眉说清「MIDI调号，已随移调平移」，不谎称文件本来就写着这个调', up2.meta);
  check(!plain.meta.includes('已随移调平移'),
    '没移调时页眉不多这句（默认输出不变）', plain.meta);

  const oct = tMeta(12);
  check(oct.meta.includes('1=Bb') && !oct.meta.includes('已随移调平移'),
    '整八度移调不改调性：仍是 1=Bb，也不声称调号平移过', oct.meta);

  const picked = tMeta(2, 2, 1);                 // 用户选 D，再 +2
  check(picked.meta.includes('手动指定 D，已随移调平移') && picked.meta.includes('1=E'),
    '手动选调 + 移调：页眉说的是用户选的 D，谱面是 1=E', picked.meta);

  const minor = tMeta(5, 9, 0);                  // 用户选 a 小调，再 +5
  check(minor.meta.includes('手动指定 A小调'),
    '小调选调后页眉写主音名「A小调」，不再写成关系大调的「C」', minor.meta);

  const f0 = fifthsOf(tXml(0));
  const f2 = fifthsOf(tXml(2));
  check(f0[0] === '-2' && f2[0] === '0',
    `MusicXML 的 <fifths> 跟着移调平移（${f0[0]} → ${f2[0]}）`, f2.join(','));

  // 试听音高必须与谱面一致：谱面移了 +2，播放还是原调就成了两首曲子
  const pitches = (semis) => {
    const p = mod._malloc(tk.length);
    mod.HEAPU8.set(tk, p);
    mod._midi2jianpu_set_transpose(semis);
    const j = JSON.parse(mod.UTF8ToString(mod._midi2jianpu_get_notes_json(p, tk.length)) || '[]');
    mod._free(p);
    return j.map((n) => n.pitch);
  };
  const base = pitches(0);
  const moved = pitches(2);
  check(base.length > 0 && base.length === moved.length &&
        moved.every((v, i) => v === base[i] + 2),
    `试听音高逐个 +2（${moved.length} 个音）`, moved.slice(0, 6).join(','));

  // 文本谱也走同一条移调路径（网页「复制简谱」按钮）
  const textOf = (semis) => {
    const p = mod._malloc(tk.length);
    mod.HEAPU8.set(tk, p);
    mod._midi2jianpu_set_transpose(semis);
    const t = mod.UTF8ToString(mod._midi2jianpu_parse(p, tk.length, -1, 1)) || '';
    mod._free(p);
    return t;
  };
  check(textOf(0) !== textOf(2) && textOf(0) === textOf(0),
    '文本谱同样按移调后的音高写（复制出来的谱子与网页一致）');

  // 幂等：同一份文件连渲染两次 0 半音，结果必须逐字节相同（不累加）
  const a1 = tMeta(0).html, a2 = tMeta(0).html;
  check(a1 === a2, '移调是「每次从原文件重算」而不是在上一次结果上累加');

  // 越界拒绝：±60 之外不生效，不静默钳位
  const back = mod._midi2jianpu_set_transpose(61);
  const now = mod._midi2jianpu_set_transpose(0);
  check(back === 0 && now === 0, 'set_transpose(61) 被拒绝，仍停在 0（不静默钳位到 60）');

  // 越界折回：+60 必然把高音推出 127，页面要如实报折回数
  tMeta(60);
  const folded = mod._midi2jianpu_transpose_folded();
  tMeta(0);
  check(folded > 0, `移调越界时上报折回音数（${folded} 个音）`);
  check(mod._midi2jianpu_transpose_folded() === 0, '回到 0 半音后折回计数清零');
}

// ---- 96. v1.165.0 网页「每行小节数」：与 CLI --bars 同口径，顺带补上打印不断行 ----
// 网页过去只有「一行约 16 拍」的自动折算：打印成歌本时行宽参差，而且基础打印
// 样式只护住 .measure、不护 .row —— 一行会被分页撕成两半，剩下的半行看着像
// 漏印。这里把 CLI --bars 的语义搬到网页（0 = 自动，1~8 = 固定），取值域一致。
{
  const bp = readFixture('sample.mid');
  const bpHtml = (perRow) => {
    const p = mod._malloc(bp.length);
    mod.HEAPU8.set(bp, p);
    mod._midi2jianpu_set_bars(perRow);
    const h = mod.UTF8ToString(mod._midi2jianpu_html(p, bp.length, -1, 1)) || '';
    mod._free(p);
    return h;
  };
  const rowsOf = (html) => html.split("<div class='row'").slice(1);
  const countsOf = (html) => rowsOf(html).map((c) => (c.match(/class='measure/g) || []).length);
  const measuresOf = (html) => (html.match(/class='measure/g) || []).length;

  check(typeof mod._midi2jianpu_set_bars === 'function',
    '网页每行小节数的导出在（否则下拉框点了没反应）');

  const auto = bpHtml(0);
  check(!auto.includes('@media print{.row{break-inside:avoid'),
    '自动折算（默认）不多出 .row 打印保护，输出与旧版一致');

  const four = bpHtml(4);
  check(four.includes('@media print{.row{break-inside:avoid'),
    '指定每行小节数后带上 .row 打印保护（一行不再被分页撕开）');

  const c4 = countsOf(four);
  check(c4.length > 0 && Math.max(...c4) <= 4 && c4.some((n) => n === 4),
    `每行最多 4 小节且确实压满过 4（各行列数 ${c4.join('/')}）`);

  // 换档位只改分行，不改内容：小节总数三种设置下必须一致
  const c2 = countsOf(bpHtml(2));
  check(measuresOf(auto) === measuresOf(four) && measuresOf(auto) === measuresOf(bpHtml(8)),
    `换每行小节数不增删小节（${measuresOf(auto)} 小节）`);
  check(c2.length > c4.length,
    `每行 2 小节的行数多于每行 4 小节（${c2.length} > ${c4.length}）`);

  // 变拍号：自动折算只认第一小节拍号，固定 N 则一段到底（CLI 注释里的原话）
  const tk = readFixture('transpose_key.mid');
  const tkCounts = (perRow) => {
    const p = mod._malloc(tk.length);
    mod.HEAPU8.set(tk, p);
    mod._midi2jianpu_set_bars(perRow);
    const h = mod.UTF8ToString(mod._midi2jianpu_html(p, tk.length, -1, 1)) || '';
    mod._free(p);
    return countsOf(h);
  };
  const tkAuto = tkCounts(0), tk3 = tkCounts(3);
  check(tk3.length > 0 && Math.max(...tk3) <= 3 && tk3.length !== tkAuto.length,
    `转调素材：自动 ${tkAuto.length} 行、固定 3 小节 ${tk3.length} 行（行宽被压平）`);

  // 文本谱与 MusicXML 不分行，不能跟着变
  const textOf = (perRow) => {
    const p = mod._malloc(bp.length);
    mod.HEAPU8.set(bp, p);
    mod._midi2jianpu_set_bars(perRow);
    const t = mod.UTF8ToString(mod._midi2jianpu_parse(p, bp.length, -1, 1)) || '';
    mod._free(p);
    return t;
  };
  const xmlOf = (perRow) => {
    const p = mod._malloc(bp.length);
    mod.HEAPU8.set(bp, p);
    mod._midi2jianpu_set_bars(perRow);
    const x = mod.UTF8ToString(mod._midi2jianpu_musicxml(p, bp.length, -1, 1)) || '';
    mod._free(p);
    return x;
  };
  check(textOf(0) === textOf(6) && xmlOf(0) === xmlOf(6),
    '每行小节数只作用于 HTML：文本谱与 MusicXML 一字不改');

  // 幂等：同一设置连渲染两次必须同字节（不在上一次结果上叠加）
  check(bpHtml(5) === bpHtml(5), '每行小节数是「每次重设」而不是累积');

  // 越界拒绝：CLI --bars 只认 1~8（0 是自动），网页同样不静默钳位
  mod._midi2jianpu_set_bars(4);
  const r9 = mod._midi2jianpu_set_bars(9);
  const rn = mod._midi2jianpu_set_bars(-1);
  check(r9 === 4 && rn === 4, `set_bars 越界被拒绝并保持原值（9→${r9}, -1→${rn}）`);

  // 复位，别把状态漏给后面的用例
  mod._midi2jianpu_set_bars(0);
  check(bpHtml(0) === auto, '复位到自动后输出回到默认版式');
}

// ---- 97. v1.166.0 网页「弱起」开关：与 CLI --pickup / --no-pickup 同源 ----
// 网页过去只能自动判定弱起，且自动判错时无从纠正（CLI 有 --pickup 覆盖）。
// 这里把三态语义搬到网页：-1 自动 / 0 关闭 / >0 手动拍数，换算与越界校验
// 共用 notation.cpp 的 applyPickupOption，绝不出现「网页与 CLI 不同谱」。
{
  const pk = readFixture('pickup.mid');
  const render = (beats) => {
    const p = mod._malloc(pk.length);
    mod.HEAPU8.set(pk, p);
    mod._midi2jianpu_set_pickup(beats);
    const h = mod.UTF8ToString(mod._midi2jianpu_html(p, pk.length, -1, 1)) || '';
    mod._free(p);
    return h;
  };
  const errOf = (beats) => {
    mod._midi2jianpu_set_pickup(beats);
    render(beats);
    return mod.UTF8ToString(mod._midi2jianpu_pickup_error()) || '';
  };

  check(typeof mod._midi2jianpu_set_pickup === 'function' &&
        typeof mod._midi2jianpu_pickup_error === 'function',
    '网页弱起开关与错误提示的导出都在（否则下拉框点了没反应）');

  const auto = render(-1);
  check(auto === render(-1), '自动弱起（默认）连渲染两次同字节（无状态累积）');

  // 无弱起：CLI --no-pickup 把 data.pickupTicks 设 0，谱面与自动不同。
  const off = render(0);
  check(off !== auto && auto.includes('弱起'),
    '关闭弱起后谱面改变且自动版确有弱起标签');
  check(errOf(0) === '', '关闭弱起不产生错误提示');

  // 手动 1 拍：与 CLI --pickup 1 走同一换算；pickup.mid 自动判的也是 1 拍，
  // 所以这时谱面与自动版应当逐字节一致（同值不同来源，结果不许分叉）。
  const one = render(1);
  check(one === auto, '手动 1 拍与自动判出的 1 拍谱面逐字节一致');
  check(errOf(1) === '', '合法手动拍数不产生错误提示');

  // 手动 2 拍：与自动不同，证明「手动值真的写进了谱面」。
  const two = render(2);
  check(two !== auto && two !== off, '手动 2 拍产生第三套谱面（值真生效）');

  // 越界：4/4 里 4 拍 = 整小节，CLI 直接 rc=1；网页无 rc，退回自动判定
  // 并给出原因，绝不把越界值静默写进谱面。
  const bad = render(4);
  check(bad === auto, '越界 4 拍退回自动判定，谱面不留下越界痕迹');
  const be = errOf(4);
  check(be.includes('整小节') || be.includes('不小于'),
    `越界拍数给出可读原因（${be.slice(0, 40)}）`);
  check(errOf(1) === '', '越界后回到合法值，错误提示被清掉');

  // 非有限值与过大拍数拒绝，返回旧值，页面可据此把下拉拉回。
  mod._midi2jianpu_set_pickup(2);
  const nan = mod._midi2jianpu_set_pickup(NaN);
  const inf = mod._midi2jianpu_set_pickup(Infinity);
  const big = mod._midi2jianpu_set_pickup(99);
  check(nan === 2 && inf === 2 && big === 2,
    `NaN/Inf/99 被拒绝并保持原值（${nan}/${inf}/${big}）`);

  // 只作用于谱面：试听音符时间不随弱起改动（弱起是记谱网格，不是播放偏移）。
  const notesOf = (beats) => {
    const p = mod._malloc(pk.length);
    mod.HEAPU8.set(pk, p);
    mod._midi2jianpu_set_pickup(beats);
    const j = mod.UTF8ToString(mod._midi2jianpu_get_notes_json(p, pk.length)) || '';
    mod._free(p);
    return j;
  };
  check(notesOf(-1) === notesOf(0), '弱起开关不改变试听音符时间（与 CLI 同口径）');

  mod._midi2jianpu_set_pickup(-1);
  check(render(-1) === auto, '复位到自动后输出回到默认版式');
}

// ---- 98. v1.167.0 网页「宫音」下拉：与 CLI --gong 同源 ----
// 五声曲自动判定的宫音会错（pentatonic.mid 自动是 G 徵调式 1=C，--gong F
// 却是 G 商调式 1=F），网页过去只有开关没有手动指定，判错无从纠正。
// 换算统一走 notation.cpp::keyFromGongTransposed，CLI 与网页不会分叉。
{
  const pt = readFixture('pentatonic.mid');
  const reset = () => {
    mod._midi2jianpu_set_transpose(0);
    mod._midi2jianpu_set_bars(0);
    mod._midi2jianpu_set_pickup(-1);
    mod._midi2jianpu_set_use_pentatonic(1);
    mod._midi2jianpu_set_gong(-1, 99);
  };
  const render = (fn) => {
    const p = mod._malloc(pt.length);
    mod.HEAPU8.set(pt, p);
    const out = mod.UTF8ToString(fn(p, pt.length, -1, 1)) || '';
    mod._free(p);
    return out;
  };
  const html = () => render(mod._midi2jianpu_html);

  check(typeof mod._midi2jianpu_set_gong === 'function',
    '宫音下拉的导出存在（否则选了没反应）');

  reset();
  const auto = html();
  check(auto.includes('G 徵调式') && auto.includes('1=C'),
    '默认自动宫音与 CLI 的 pentatonic.mid 判定一致');
  check(mod._midi2jianpu_set_gong(-1, 99) === -1 && html() === auto,
    '自动宫音连渲染两次同字节（无状态累积）');

  check(mod._midi2jianpu_set_gong(5, 99) === 5, 'set_gong(5) 返回实际生效值');
  const f = html();
  check(f !== auto && f.includes('手动指定宫音 F') && f.includes('G 商调式'),
    'F 宫：页眉措辞与调式名都与 CLI --gong F 同口径');
  check(html() === f, '同一宫音连渲染两次同字节');

  check(mod._midi2jianpu_set_gong(1, -5) === 1, 'Db 宫（降号侧 hint）被接受');
  const db = html();
  check(db !== f && db.includes('宫音 Db'),
    '降号写法被记住，不查表写成 C#');

  // 三个输出入口都要响应同一个宫音，不能只改 HTML。
  reset();
  const aText = render(mod._midi2jianpu_parse);
  const aXml = render(mod._midi2jianpu_musicxml);
  mod._midi2jianpu_set_gong(5, 99);
  const fText = render(mod._midi2jianpu_parse);
  const fXml = render(mod._midi2jianpu_musicxml);
  // 文本谱页眉不写 keySourceNote（那是 HTML 专用），只写调号与调式名。
  check(fText !== aText && fText.includes('1=F') && fText.includes('宫音 F'),
    '文本谱也随宫音改变，页眉调号与调式名一起变');
  check(fXml !== aXml && fXml.includes('<fifths>-1</fifths>'),
    'MusicXML 也随宫音改变，调号写成 F 的 -1');

  // 宫音是「原调」：移调时宫音与升降号侧一起走，页眉保留用户选的原名。
  reset();
  mod._midi2jianpu_set_gong(5, 99);
  mod._midi2jianpu_set_transpose(2);
  const shifted = html();
  check(shifted.includes('手动指定宫音 F，已随移调平移') && shifted.includes('1=G'),
    '手动宫音随移调平移，页眉仍写用户选的原宫音');
  check(shifted !== f, '移调后的谱面与原调不同');

  // 越界不静默钳位：返回旧值，谱面保持上一次合法选择。
  reset();
  mod._midi2jianpu_set_gong(5, 99);
  check(mod._midi2jianpu_set_gong(12, 99) === 5 && html() === f,
    'pc=12 越界被拒绝，仍用原宫音');
  check(mod._midi2jianpu_set_gong(-2, 99) === 5 &&
        mod._midi2jianpu_set_gong(5, 8) === 5,
    'pc=-2 / hint=8 越界都被拒绝');

  reset();
  check(html() === auto, '复位后输出回到默认判定');
}

// ---- 99. v1.168.0 网页「时值量化」开关：与 CLI --no-quantize 同源 ----
// 人演奏的 note-off 普遍提前，自动量化把 humanized.mid 的附点八分吸附成
// 普通八分；CLI 有 --no-quantize 出口，网页过去强制量化无从还原原始时值。
// 两个入口调用的仍是同一个 m2j::quantizeTiming()，只差一个布尔。
{
  const hm = readFixture('humanized.mid');
  const reset = () => {
    mod._midi2jianpu_set_transpose(0);
    mod._midi2jianpu_set_bars(0);
    mod._midi2jianpu_set_pickup(-1);
    mod._midi2jianpu_set_gong(-1, 99);
    mod._midi2jianpu_set_quantize(1);
  };
  const render = (fn) => {
    const p = mod._malloc(hm.length);
    mod.HEAPU8.set(hm, p);
    const out = mod.UTF8ToString(fn(p, hm.length, -1, 1)) || '';
    mod._free(p);
    return out;
  };
  const html = () => render(mod._midi2jianpu_html);

  check(typeof mod._midi2jianpu_set_quantize === 'function' &&
        typeof mod._midi2jianpu_quantize_adjusted === 'function',
    '量化开关与修正数报告两个导出都在');

  reset();
  const on = html();
  const adjusted = mod._midi2jianpu_quantize_adjusted();
  check(adjusted > 0, '默认开量化时如实报告修正音符数');
  check(html() === on && mod._midi2jianpu_quantize_adjusted() === adjusted,
    '同开关连渲染两次同字节、修正数不累积');

  check(mod._midi2jianpu_set_quantize(0) === 0, 'set_quantize(0) 返回实际生效值');
  const off = html();
  check(off !== on && off.includes('='),
    '关量化后谱面改变，保留原始附点时值');
  check(mod._midi2jianpu_quantize_adjusted() === 0,
    '关量化后修正数归零，状态行不顶上一版的数字');
  check(html() === off, '关量化连渲染两次同字节');

  // 三个渲染入口都要响应同一个开关，不能只改 HTML。
  // 注意先拨回开：上面几步把开关留在了「关」，直接取 on* 会拿到关量化版。
  mod._midi2jianpu_set_quantize(1);
  const onText = render(mod._midi2jianpu_parse);
  const onXml = render(mod._midi2jianpu_musicxml);
  mod._midi2jianpu_set_quantize(0);
  const offText = render(mod._midi2jianpu_parse);
  const offXml = render(mod._midi2jianpu_musicxml);
  check(offText !== onText && offXml !== onXml,
    '文本谱与 MusicXML 也随量化开关改变');

  // 越界/任意整数按布尔口径接受；非 0 一律开，与 set_use_* 一致。
  check(mod._midi2jianpu_set_quantize(7) === 1 && html() === on,
    '非 0 值按「开」处理，谱面与默认量化一致');

  reset();
  check(html() === on, '复位后输出回到默认量化版式');
}

// ---- 100. v1.169.0 网页「文本谱小节号」：与 CLI --bar-numbers 同源 ----
// CLI `--bar-numbers` 给复制出去的文本谱每行开头加 [N] 前缀；网页此前复制
// 简谱拿到的是无编号文本，长谱按小节定位无从下手。formatJianpu 第 3 参
// 本就支持，这里只把布尔透传到 parse 入口，HTML/XML 不受影响（本就印小节号）。
{
  const sp = readFixture('sample.mid');
  const render = (fn) => {
    const p = mod._malloc(sp.length);
    mod.HEAPU8.set(sp, p);
    const out = mod.UTF8ToString(fn(p, sp.length, -1, 1)) || '';
    mod._free(p);
    return out;
  };

  check(typeof mod._midi2jianpu_set_bar_numbers === 'function',
    '文本小节号开关的导出存在');

  mod._midi2jianpu_set_bar_numbers(0);
  const plain = render(mod._midi2jianpu_parse);
  check(plain.indexOf('[2]') < 0, '默认关：文本谱没有行首小节号');

  check(mod._midi2jianpu_set_bar_numbers(1) === 1, 'set_bar_numbers(1) 返回生效值');
  const numbered = render(mod._midi2jianpu_parse);
  check(numbered !== plain && numbered.includes('[2]') && numbered.includes('[3]'),
    '开：文本谱每行开头出现 [2] [3] 小节号');
  check(render(mod._midi2jianpu_parse) === numbered, '同开关连出两次同字节');

  // 只作用于文本谱：HTML/XML 不受这个开关影响，与 CLI 的警告口径一致。
  const htmlOn = render(mod._midi2jianpu_html);
  const xmlOn = render(mod._midi2jianpu_musicxml);
  mod._midi2jianpu_set_bar_numbers(0);
  check(render(mod._midi2jianpu_html) === htmlOn &&
        render(mod._midi2jianpu_musicxml) === xmlOn,
    'HTML 与 MusicXML 不随此开关改变（本就无条件印小节号）');

  check(mod._midi2jianpu_set_bar_numbers(7) === 1 &&
        render(mod._midi2jianpu_parse) === numbered,
    '非 0 值按「开」处理');
  mod._midi2jianpu_set_bar_numbers(0);
  check(render(mod._midi2jianpu_parse) === plain, '复位后文本谱回到无编号版');
}

// ---- 101. v1.170.0 网页「中途转调」：与 CLI --no-key-changes 同源 ----
// CLI 关掉转调后全曲按曲首一个调记谱：文本页眉不写转调链、HTML 不标
// 「转 1=D」、MusicXML 只写一次 <key>。此前网页只读 key_change_count
// 做提示，没有任何开关能真正关掉转调。这里守三端一致，并守无转调文件
// 逐字节不变（这正是默认开启的零回归要求）。
{
  const mod_ = readFixture('modulation.mid');
  const sample = readFixture('sample.mid');
  const render = (bytes, fn) => {
    const p = mod._malloc(bytes.length);
    mod.HEAPU8.set(bytes, p);
    const out = mod.UTF8ToString(fn(p, bytes.length, -1, 1)) || '';
    mod._free(p);
    return out;
  };

  check(typeof mod._midi2jianpu_set_key_changes === 'function',
    '中途转调开关的导出存在');
  check(mod._midi2jianpu_set_key_changes(1) === 1,
    'set_key_changes(1) 返回生效值');

  const onHtml = render(mod_, mod._midi2jianpu_html);
  const onText = render(mod_, mod._midi2jianpu_parse);
  const onXml = render(mod_, mod._midi2jianpu_musicxml);
  check(onHtml.includes('1=C') && onHtml.includes('转 1=D'),
    '开：HTML 页眉有转调链、变化小节标「转 1=D」');

  mod._midi2jianpu_set_key_changes(0);
  const offHtml = render(mod_, mod._midi2jianpu_html);
  const offText = render(mod_, mod._midi2jianpu_parse);
  const offXml = render(mod_, mod._midi2jianpu_musicxml);
  check(!offHtml.includes('转 1=D') && offHtml.indexOf('1=C 大调 →') < 0,
    '关：HTML 没有转调链与转调标签，全曲一个调');
  check(offHtml !== onHtml && offText !== onText && offXml !== onXml,
    '文本 / HTML / MusicXML 三端都随开关改变，不会只改一端');
  const keysOn = (onXml.match(/<key>/g) || []).length;
  const keysOff = (offXml.match(/<key>/g) || []).length;
  check(keysOff < keysOn && keysOff === 2,
    '关：MusicXML 每个 part 只写一次 <key>（开时 ' + keysOn + ' 个，关时 ' + keysOff + ' 个）');

  // 无转调文件关掉开关必须逐字节不变：默认开启时一个字节都没动。
  const sOn = [render(sample, mod._midi2jianpu_parse),
               render(sample, mod._midi2jianpu_html),
               render(sample, mod._midi2jianpu_musicxml)];
  const sOff = [render(sample, mod._midi2jianpu_parse),
                render(sample, mod._midi2jianpu_html),
                render(sample, mod._midi2jianpu_musicxml)];
  check(sOn.every((v, i) => v === sOff[i]),
    '无转调文件（sample.mid）开关前后三端逐字节一致');

  check(mod._midi2jianpu_set_key_changes(7) === 1 &&
        render(mod_, mod._midi2jianpu_html) === onHtml,
    '非 0 值按「开」处理，谱面与默认一致');
  mod._midi2jianpu_set_key_changes(1);
  check(render(mod_, mod._midi2jianpu_html) === onHtml,
    '复位后输出回到默认开启版');
}

// ---- 102. v1.171.0 关掉内置歌词后提示改口，与 CLI --no-midi-lyrics 同源 ----
// 此前 midiLyricHint() 完全不看 g_useMidiLyrics：关掉后谱面已经没词了，
// 提示栏却还说「此文件自带 N 个歌词音节（轨 1）」，选到无词轨甚至诱导
// 「把轨道选回自动才会用上」。CLI 这时早已改口「已忽略 … N 个事件」。
{
  const kb = readFixture('karaoke.mid');
  const hint = (on, track) => {
    mod._midi2jianpu_set_use_midi_lyrics(on ? 1 : 0);
    mod._midi2jianpu_set_track(track);
    const p = mod._malloc(kb.length);
    mod.HEAPU8.set(kb, p);
    const s = mod.UTF8ToString(mod._midi2jianpu_midi_lyric_info(p, kb.length)) || '';
    mod._free(p);
    return s;
  };

  const on = hint(true, -1), off = hint(false, -1);
  check(/自带 \d+ 个歌词音节/.test(on), '开：仍提示自带音节（原行为不变）', on);
  check(/已忽略 MIDI 内置歌词: \d+ 个事件/.test(off), '关：与 CLI 同口径提示已忽略', off);
  check(!off.includes('自带') && !off.includes('才会用上'),
    '关：不再说自带、不再诱导换轨', off);
  check(on !== off, '开关确实改变提示文案');

  // 关态切到无词轨：必须是同一句「已忽略」，不能因轨道不同又说回旧话
  const offOther = hint(false, 2);
  check(/已忽略/.test(offOther) && !offOther.includes('才会用上'),
    '关+无词轨：仍提示已忽略，不诱导换轨', offOther);

  check(/自带/.test(hint(true, -1)), '复位开态');
}

// ---- 103. v1.172.0 宫音提示编码两位宽：gong=10/11 不再串位 ----
// detectPentatonic 枚举 12 个宫音，gongPc 能到 10(A#)/11(B)。旧编码
// final*100+gong*10+mode 在 gong>=10 时向百位进位，网页解码出的宫音与
// 终止音全错（B宫徵调式被说成「1=C# / G徵」，与谱面页眉「1=B」矛盾）。
// 这里现场拼两份五声单轨 SMF，验证新编码 final*10000+gong*100+mode
// 解出的值与 CLI 页眉真值一致；不动谱面输出。
{
  // gong 宫五声（宫商角徵羽 = 0/2/4/7/9），末音落 finalRel 指定的正音
  const pentMidi = (gong, finalRel) => {
    const pcs = [0, 2, 4, 7, 9].map((d) => (gong + d) % 12);
    const seq = pcs.concat(pcs.slice().reverse(), pcs, [(gong + finalRel) % 12]);
    let body = [0x00, 0xff, 0x03, 0x06, 0x43, 0x6f, 0x6e, 0x64, 0x75, 0x63, 0x74,
                0x00, 0xff, 0x51, 0x03, 0x07, 0xa1, 0x20,
                0x00, 0xff, 0x58, 0x04, 0x04, 0x02, 0x18, 0x08];
    let tick = 0;
    const vlq = (n) => { const b = [n & 0x7f]; n >>= 7; while (n) { b.unshift((n & 0x7f) | 0x80); n >>= 7; } return b; };
    for (const pc of seq) {
      const pitch = 60 + pc;
      body = body.concat(vlq(tick === 0 ? 0 : 0), [0x90, pitch, 0x5a]);
      body = body.concat(vlq(480), [0x80, pitch, 0x00]);
      tick += 480;
    }
    body = body.concat(vlq(0), [0xff, 0x2f, 0x00]);
    const chunk = new Uint8Array(8 + body.length);
    chunk.set([0x4d, 0x54, 0x72, 0x6b], 0);
    new DataView(chunk.buffer).setUint32(4, body.length);
    chunk.set(body, 8);
    const all = new Uint8Array(14 + chunk.length);
    all.set([0x4d, 0x54, 0x68, 0x64], 0);
    const hv = new DataView(all.buffer, 4, 10);
    hv.setUint32(0, 6); hv.setUint16(4, 1); hv.setUint16(6, 1); hv.setUint16(8, 480);
    all.set(chunk, 14);
    return all;
  };

  const query = (bytes) => {
    mod._midi2jianpu_set_use_pentatonic(1);
    mod._midi2jianpu_set_gong(-1, 99);
    mod._midi2jianpu_set_transpose(0);
    const p = mod._malloc(bytes.length); mod.HEAPU8.set(bytes, p);
    const info = mod._midi2jianpu_pentatonic_info(p, bytes.length);
    mod._free(p);
    return info;
  };
  // 网页解码口径（与 index.html 逐字一致）
  const decode = (pi) => ({ fin: Math.floor(pi / 10000),
                            gong: Math.floor(pi / 100) % 100, md: pi % 100 });

  const bGong = query(pentMidi(11, 7));   // B宫徵(7)、终止 pc=6
  const b = decode(bGong);
  check(b.gong === 11 && b.fin === 6 && b.md === 5,
    'B宫(11)徵调式解出宫音11/终止6/徵5', `${bGong} -> ${JSON.stringify(b)}`);

  const aGong = query(pentMidi(10, 9));   // A#宫羽(9)、终止 pc=7
  const a = decode(aGong);
  check(a.gong === 10 && a.fin === 7 && a.md === 6,
    'A#宫(10)羽调式解出宫音10/终止7/羽6', `${aGong} -> ${JSON.stringify(a)}`);

  // 低宫音仍按原有语义，只是整体放大到两位宽
  const cGong = query(pentMidi(0, 7));    // C宫徵、终止 pc=7
  const c = decode(cGong);
  check(c.gong === 0 && c.fin === 7 && c.md === 5,
    'C宫(0)徵调式解出宫音0/终止7/徵5', `${cGong} -> ${JSON.stringify(c)}`);
}

// ---- 104. v1.175.0 歌词格式导出：网页不再对 JSON 谎称 LRC tempo map ----
// CLI 只在 LRC 时说「歌词秒数已按 tempo map 分段换算」；这个判定与
// lyric_info 里的「按 LRC / 按 JSON 解析」同走 detectLyricFormat("")。
{
  check(typeof mod._midi2jianpu_lyric_format === 'function',
    'WASM 产物带 _midi2jianpu_lyric_format 导出（v1.175.0）');
  const lyricFormat = (text) => {
    const n = mod.lengthBytesUTF8(text) + 1;
    const p = mod._malloc(n);
    mod.stringToUTF8(text, p, n);
    const v = mod._midi2jianpu_lyric_format(p);
    mod._free(p);
    return v;
  };
  const lrcText = '[00:00.00]春\n[00:01.00]江';
  const jsonText = '[{"start_beat":0,"text":"春"}]';
  check(lyricFormat(lrcText) === 1, 'LRC 秒时间戳判为 1');
  check(lyricFormat(jsonText) === 2, 'JSON start_beat 判为 2');
  check(lyricFormat('  ' + lrcText) === 1, '前导空白的 LRC 仍判 1（不靠首字符猜）');
  check(lyricFormat('[{"text":"春","start_beat":0}]') === 2,
    '字段顺序颠倒的 JSON 仍判 2');

  const sample = readFixture('sample.mid');
  const lyricInfoOf = (text) => {
    const p = mod._malloc(sample.length);
    mod.HEAPU8.set(sample, p);
    const n = mod.lengthBytesUTF8(text) + 1;
    const lp = mod._malloc(n);
    mod.stringToUTF8(text, lp, n);
    const s = mod._midi2jianpu_lyric_info(p, sample.length, lp);
    const out = s ? mod.UTF8ToString(s) : '';
    mod._free(lp);
    mod._free(p);
    return out;
  };
  check(lyricFormat(lrcText) === 1 && lyricInfoOf(lrcText).includes('按 LRC 解析'),
    '同一 LRC：format 导出与 lyric_info 说法一致');
  check(lyricFormat(jsonText) === 2 && lyricInfoOf(jsonText).includes('按 JSON 解析'),
    '同一 JSON：format 导出与 lyric_info 说法一致');
}

// ---- 105. v1.176.0 试听数据与时值量化同源：关掉量化后耳朵听到的也变 ----
{
  const human = readFixture('humanized.mid');
  const firstPlayNote = (on) => {
    mod._midi2jianpu_set_track(-1);
    mod._midi2jianpu_set_quantize(on);
    const p = mod._malloc(human.length);
    mod.HEAPU8.set(human, p);
    mod._midi2jianpu_get_notes_json(p, human.length);
    const len = mod._midi2jianpu_result_len();
    const b = mod._malloc(len + 1);
    mod._midi2jianpu_copy_result(b, len + 1);
    const txt = mod.UTF8ToString(b);
    mod._free(p);
    mod._free(b);
    return JSON.parse(txt)[0];
  };
  // humanized.mid：division=960、♩=72；量化开首音 480 tick=0.417s，
  // 关掉后保留原 408 tick=0.354s（CLI --player 的同口径真值）。
  const on = firstPlayNote(1);
  const off = firstPlayNote(0);
  firstPlayNote(1); // 复位，别影响后面的用例
  check(Math.abs(on.duration - 0.417) < 0.002,
    '量化开：试听首音 0.417s（CLI 播放器 480 tick）', JSON.stringify(on));
  check(Math.abs(off.duration - 0.354) < 0.002,
    '量化关：试听首音 0.354s（CLI 播放器 408 tick）', JSON.stringify(off));
  check(on.duration !== off.duration, '量化开关确实改变试听时值',
    `${on.duration} vs ${off.duration}`);
  check(on.pitch === off.pitch && on.velocity === off.velocity,
    '量化只改时值，不改音高与力度');
}

// ---- 106. v1.176.0 试听音高同样吃弯音修正，与三端谱面共用一个函数 ----
{
  const bendMidi = readFixture('bendrange_channels.mid');
  const p = mod._malloc(bendMidi.length);
  mod.HEAPU8.set(bendMidi, p);
  mod._midi2jianpu_set_track(-1);
  mod._midi2jianpu_get_notes_json(p, bendMidi.length);
  const len = mod._midi2jianpu_result_len();
  const b = mod._malloc(len + 1);
  mod._midi2jianpu_copy_result(b, len + 1);
  const notes = JSON.parse(mod.UTF8ToString(b));
  mod._free(p);
  mod._free(b);
  check(notes.length === 2, '弯音素材试听数据仍有两个音', JSON.stringify(notes));
  check(notes[0].pitch === 72 && notes[1].pitch === 62,
    '试听音高 72 / 62（与 CLI --player 的谱面音高一致）',
    JSON.stringify(notes.map((n) => n.pitch)));
}

// ---- 107. v1.180.0 CC121 清空 RPN 选择器（孤立 CC6 不再改弯音范围）----
// RP-015：CC121 把 RPN/NRPN 选择器置 127/127，已存参数值不变。旧版只清踏板，
// RPN 还停在 0/0，其后孤立 CC6=12 被当成弯音灵敏度（±2 变 ±12），满弯音的
// C4 从 D（简谱 2）记成高八度的 C（简谱 1'）。CLI 冒烟 [157] 是同一份输入。
{
  const be32 = (n) => [(n >>> 24) & 255, (n >>> 16) & 255, (n >>> 8) & 255, n & 255];
  const vlq = (n) => { const o = []; let b = n & 0x7f; n >>= 7; while (n) { o.unshift((b | 0x80) & 0xff); b = n & 0x7f; n >>= 7; } o.push(b); return o; };
  const rpnMidi = (explicitNull) => {
    const body = [
      ...vlq(0), 0xb0, 101, 0, ...vlq(0), 0xb0, 100, 0,   // 选中 RPN 0
      ...vlq(0), 0xb0, 121, 0,                             // CC121
    ];
    if (explicitNull)
      body.push(...vlq(0), 0xb0, 101, 127, ...vlq(0), 0xb0, 100, 127);
    body.push(
      ...vlq(0), 0xb0, 6, 12,                              // 孤立 CC6
      ...vlq(0), 0xe0, 0x7f, 0x7f,                         // 满弯音
      ...vlq(0), 0x90, 60, 100, ...vlq(480), 0x80, 60, 0,
      ...vlq(0), 0xff, 0x2f, 0x00);
    const head = [0x4d, 0x54, 0x68, 0x64, ...be32(6), 0, 1, 0, 1, 0x01, 0xe0];
    return new Uint8Array([...head, 0x4d, 0x54, 0x72, 0x6b, ...be32(body.length), ...body]);
  };
  const parse107 = (bytes) => {
    const p = mod._malloc(bytes.length);
    mod.HEAPU8.set(bytes, p);
    const s = mod._midi2jianpu_parse(p, bytes.length, 0, 1);
    const out = s ? mod.UTF8ToString(s) : '';
    mod._free(p);
    return out;
  };
  const byCc121 = parse107(rpnMidi(false));
  const explicitNull = parse107(rpnMidi(true));
  check(!byCc121.includes('弯音范围'),
    'WASM CC121 后孤立 CC6 不再产生弯音范围设定', byCc121.slice(0, 120));
  // 同页前面几组改过量化等全局态，音符的延长线格式可能不同；只钉音高：
  // 第一个音必须是 2（默认 ±2），绝不能是 1'（±12 的高八度 C）。
  check(/\b2[\s\-]/.test(byCc121) && !byCc121.includes("1'"),
    'WASM 满弯音音符记为 2（±2），不再错成 1\'（±12）', byCc121.slice(-80));
  check(byCc121 === explicitNull,
    'WASM CC121 与显式 127/127 退出 RPN 文本谱逐字节相同',
    `cc121=${JSON.stringify(byCc121.slice(-30))} null=${JSON.stringify(explicitNull.slice(-30))}`);
}

console.log('');
if (failures.length) {
  console.log(`X ${failures.length} 项失败`);
  process.exit(1);
}
console.log('全部通过');
