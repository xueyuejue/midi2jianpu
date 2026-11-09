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

console.log('');
if (failures.length) {
  console.log(`X ${failures.length} 项失败`);
  process.exit(1);
}
console.log('全部通过');
