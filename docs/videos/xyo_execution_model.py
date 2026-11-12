"""xyo-cpp runtime architecture: Japanese captioned Manim explainer.

Render:
  /home/kicky/dev/scratch/hikkaku/examples/manim2scratch/.venv/bin/python \
    -m manim -qm -r 1280,720 docs/videos/xyo_execution_model.py \
    XyoExecutionModel -o xyo_execution_model.mp4 --media_dir docs/videos/media

This video intentionally does not claim full WASM/LLVM feature parity or
general on-stack replacement. Based on docs/design.md, RUNNING.md,
docs/jit-osr-2026-11-06.md, docs/wasm-hotspot.md,
docs/jit-backend-compatibility.md, and docs/scratch-unified-source-ir.md.
"""

from manim import (
    Scene, Text, VGroup, RoundedRectangle, Rectangle, Line, Arrow, Circle,
    Dot, FadeIn, FadeOut, GrowArrow, Create, Write, Transform, Indicate,
    LaggedStart, UP, DOWN, LEFT, RIGHT, ORIGIN, config,
)

FONT = "Noto Sans CJK JP"
BG = "#081121"
CARD = "#13243A"
CARD2 = "#172B43"
BORDER = "#36516C"
WHITE = "#EFF6FF"
MUTED = "#A9BACF"
DIM = "#7188A5"
CYAN = "#4DE1CB"
BLUE = "#68A9FF"
ORANGE = "#FFBC75"
PURPLE = "#BEA1FF"
PINK = "#FF93B4"
GREEN = "#83E8A3"
YELLOW = "#F4D277"
BAD = "#FD8595"

config.background_color = BG


def txt(value: str, size: int = 25, color: str = WHITE, bold: bool = False):
    return Text(
        value, font=FONT, font_size=size, color=color,
        weight="BOLD" if bold else "NORMAL", line_spacing=0.8,
    )


def card(x, y, width, height, title, detail="", accent=BLUE, title_size=26, body_size=19):
    panel = RoundedRectangle(
        corner_radius=0.18, width=width, height=height,
        fill_color=CARD, fill_opacity=1,
        stroke_color=accent, stroke_opacity=0.65, stroke_width=2,
    ).move_to([x, y, 0])
    stripe = Rectangle(width=0.07, height=height - 0.32,
                       fill_color=accent, fill_opacity=1, stroke_width=0)
    stripe.move_to(panel.get_left() + RIGHT * 0.18)
    title_obj = txt(title, title_size, WHITE, True)
    detail_obj = txt(detail, body_size, MUTED) if detail else None
    title_obj.move_to([x + 0.09, y + (0.27 if detail else 0), 0])
    group = VGroup(panel, stripe, title_obj)
    if detail_obj is not None:
        detail_obj.move_to([x + 0.09, y - 0.28, 0])
        group.add(detail_obj)
    return group


def link(a, b, color=CYAN, width=3):
    return Arrow(a, b, color=color, stroke_width=width,
                 buff=0.09, max_tip_length_to_length_ratio=0.18)


def badge(value, x, y, color=BLUE, width=None):
    label = txt(value, 18, color, True)
    w = width if width is not None else max(1.55, label.width + 0.45)
    box = RoundedRectangle(
        corner_radius=0.16, height=0.43, width=w,
        fill_color=CARD2, fill_opacity=0.96,
        stroke_color=color, stroke_width=1, stroke_opacity=0.5,
    ).move_to([x, y, 0])
    label.move_to(box)
    return VGroup(box, label)


class XyoExecutionModel(Scene):
    """A diagram-led explainer, no external video/image assets or LaTeX."""

    def clear_page(self):
        if self.mobjects:
            self.play(*[FadeOut(o) for o in list(self.mobjects)], run_time=0.44)

    def heading(self, index: int, title: str, subtitle: str):
        self.clear_page()
        overline = txt(f"{index:02d}   /   07", 19, CYAN, True).move_to([-5.67, 3.42, 0])
        heading = txt(title, 37, WHITE, True)
        heading.move_to([-5.85 + heading.width / 2, 2.89, 0])
        sub = txt(subtitle, 19, MUTED)
        sub.move_to([-5.85 + sub.width / 2, 2.38, 0])
        rule = Line([-6.0, 2.07, 0], [6.0, 2.07, 0],
                    stroke_width=1.3, color=BORDER)
        self.play(FadeIn(overline), FadeIn(heading, shift=UP * 0.14),
                  FadeIn(sub), Create(rule), run_time=0.75)

    def footer(self, index: int, line: str):
        band = RoundedRectangle(
            corner_radius=0.08, width=12.3, height=0.64,
            fill_color="#0C1C30", fill_opacity=1, stroke_width=0,
        ).move_to([0, -3.17, 0])
        caption = txt(line, 18, WHITE)
        if caption.width > 11.7:
            caption.scale(11.7 / caption.width)
        caption.move_to([0, -3.16, 0])
        track = Line([-6.0, -3.73, 0], [6.0, -3.73, 0],
                     color="#253952", stroke_width=3.0)
        progress = Line([-6.0, -3.73, 0],
                        [-6.0 + 12.0 * index / 7, -3.73, 0],
                        color=CYAN, stroke_width=4.0)
        self.play(FadeIn(VGroup(band, caption, track, progress)), run_time=0.30)

    def construct(self):
        self.intro()
        self.pipeline()
        self.modes()
        self.compiler()
        self.scheduler()
        self.safety()
        self.experimental()
        self.backends()
        self.outro()

    def intro(self):
        grid = VGroup()
        for x in range(-7, 8):
            grid.add(Line([x, -3.9, 0], [x, 3.9, 0],
                          stroke_width=0.55, color="#1B3652").set_opacity(0.27))
        for y in range(-4, 5):
            grid.add(Line([-7.1, y, 0], [7.1, y, 0],
                          stroke_width=0.55, color="#1B3652").set_opacity(0.27))
        label = txt("SCRATCH RUNTIME ARCHITECTURE", 20, CYAN, True).move_to([0, 2.25, 0])
        title = txt("xyo-cpp", 86, WHITE, True).move_to([0, 1.15, 0])
        underline = Line([-2.95, 0.57, 0], [2.95, 0.57, 0],
                         color=CYAN, stroke_width=4)
        desc = txt("Scratchをどうやって高速に実行するのか", 30, MUTED)
        desc.move_to([0, -0.02, 0])
        routes = [
            badge("Interpreter", -3.9, -1.48, ORANGE, 3.0),
            badge("LLVM JIT", 0.0, -1.48, CYAN, 3.0),
            badge("AOT", 3.9, -1.48, PURPLE, 3.0),
        ]
        note = txt("ネイティブC++  ×  イベント駆動  ×  段階的コンパイル", 20, DIM)
        note.move_to([0, -2.64, 0])
        self.play(FadeIn(grid), FadeIn(label, shift=UP*0.2), run_time=0.7)
        self.play(FadeIn(title, scale=0.84), Create(underline), run_time=0.95)
        self.play(FadeIn(desc), LaggedStart(*[FadeIn(r, shift=UP * 0.22)
                                            for r in routes], lag_ratio=0.18),
                  run_time=0.95)
        self.play(FadeIn(note), run_time=0.35)
        self.wait(1.9)

    def pipeline(self):
        self.heading(1, "Scratchから実行へ", "プロジェクトを読み込み、スクリプトを解析する")
        boxes = [
            card(-4.70, 0.73, 2.70, 1.38, "project.sb3", "Scratchプロジェクト", ORANGE, 25),
            card(-1.55, 0.73, 2.70, 1.38, "SB3 Loader", "ブロック・変数を構築", BLUE, 25),
            card(1.60, 0.73, 2.70, 1.38, "Source IR", "構造と副作用を解析", PURPLE, 25),
            card(4.75, 0.73, 2.70, 1.38, "Runtime", "実行・描画・音声", CYAN, 25),
        ]
        arrows = [
            link([-3.29, .73, 0], [-2.99, .73, 0]),
            link([-.14, .73, 0], [.16, .73, 0]),
            link([3.01, .73, 0], [3.31, .73, 0]),
        ]
        self.play(LaggedStart(*[FadeIn(b, shift=UP*.18)
                                for b in boxes], lag_ratio=.15), run_time=1.2)
        self.play(*[GrowArrow(a) for a in arrows], run_time=.5)
        note = card(0, -1.32, 10.5, 1.15,
                    "解析結果から実行経路を選択",
                    "Interpreter / LLVM JIT / AOT", CYAN, 25, 20)
        self.play(FadeIn(note, shift=UP*.2), run_time=.6)
        warning = txt("共通IRは段階導入中。すべての命令が共通SSAになるわけではない", 18, YELLOW)
        warning.move_to([0, -2.32, 0])
        self.play(FadeIn(warning), run_time=.35)
        self.footer(1, "ロード後は、同じScratchの意味論を複数の実行方法で処理する。")
        self.wait(2.2)

    def modes(self):
        self.heading(2, "JITを使うタイミング", "通常の実行モードは3つ。既定はHotSpot JIT")
        boxes = [
            card(-4.52, .75, 3.55, 1.65, "OFF", "ずっとInterpreter", ORANGE),
            card(0, .75, 3.55, 1.65, "Eager", "実行前に一括コンパイル", BLUE),
            card(4.52, .75, 3.55, 1.65, "HotSpot", "熱いスクリプトだけJIT", CYAN),
        ]
        self.play(LaggedStart(*[FadeIn(b, shift=UP*.13)
                                for b in boxes], lag_ratio=.18), run_time=1.05)
        p1 = txt("Interpreter", 24, ORANGE, True).move_to([-4.25, -1.0, 0])
        p2 = txt("実行回数 / ループの熱さ", 22, MUTED).move_to([-0.45, -1.0, 0])
        track = RoundedRectangle(
            width=6.25, height=0.29, corner_radius=0.13,
            stroke_color=BORDER, stroke_width=1,
            fill_color=CARD, fill_opacity=1,
        ).move_to([0, -1.58, 0])
        fill = Rectangle(width=0.12, height=0.21, fill_color=CYAN,
                         fill_opacity=1, stroke_width=0)
        fill.move_to([-3.02, -1.58, 0])
        threshold = Line([1.5, -1.85, 0], [1.5, -1.28, 0],
                         color=YELLOW, stroke_width=2)
        hot = txt("JITへ昇格", 23, CYAN, True).move_to([4.5, -1.58, 0])
        self.play(FadeIn(p1), FadeIn(p2), FadeIn(track), FadeIn(fill), run_time=.6)
        self.play(FadeIn(threshold), run_time=.25)
        target_fill = Rectangle(width=5.75, height=.21, stroke_width=0,
                                fill_color=CYAN, fill_opacity=1)
        target_fill.move_to([-.13, -1.58, 0])
        self.play(Transform(fill, target_fill), run_time=1.45)
        self.play(FadeIn(hot, shift=LEFT*.18), Indicate(boxes[2][0], color=CYAN),
                  run_time=.65)
        self.footer(2, "HotSpotは最初にインタプリタで走り、頻繁に使う部分だけ機械語にする。")
        self.wait(1.9)

    def compiler(self):
        self.heading(3, "LLVMが機械語を作る", "HotSpotが選んだスクリプトをネイティブコードへ")
        steps = [
            card(-4.77, 0.68, 2.46, 1.42, "Scratch", "スクリプト", ORANGE),
            card(-1.58, 0.68, 2.46, 1.42, "LLVM IR", "数値・条件・ループ", BLUE),
            card(1.60, 0.68, 2.46, 1.42, "O3", "LLVM最適化", PURPLE),
            card(4.77, 0.68, 2.46, 1.42, "ORC JIT", "CPUの機械語", CYAN),
        ]
        self.play(LaggedStart(*[FadeIn(b) for b in steps],
                                lag_ratio=.18), run_time=1.1)
        arrows = [link([-3.5,.68,0],[-2.86,.68,0]),
                  link([-.3,.68,0],[.36,.68,0]),
                  link([2.88,.68,0],[3.55,.68,0])]
        self.play(*[GrowArrow(a) for a in arrows], run_time=.52)
        self.play(Indicate(steps[3][0], color=CYAN, scale_factor=1.06), run_time=.70)
        label = txt("計算が得意な経路はインライン化・型特化・SSA最適化", 26, WHITE)
        label.move_to([0, -1.05, 0])
        runtime = card(0, -2.02, 9.0, .90, "Runtime ABI / Scratch互換処理",
                       "数値変換・リスト・文字列・GC・ブロックの副作用", BLUE, 22, 18)
        self.play(FadeIn(label), FadeIn(runtime, shift=UP*.17), run_time=.7)
        self.footer(3, "JITはScratchの全処理を独自実装するのではなく、共通ランタイムも呼び出す。")
        self.wait(2.2)

    def scheduler(self):
        self.heading(4, "実行を止め、また再開する", "Scratchの並行スクリプトはSequencerが管理")
        rows = [
            card(-3.30, 1.15, 5.85, .93, "Thread A  /  計算",
                 "ループ → yield → 続きへ", ORANGE, 23, 17),
            card(-3.30, -.12, 5.85, .93, "Thread B  /  待機",
                 "wait → スケジューラへ", BLUE, 23, 17),
            card(-3.30, -1.39, 5.85, .93, "Thread C  /  イベント",
                 "broadcast → 受信スクリプト", PURPLE, 23, 17),
        ]
        hub = card(3.80, -.12, 3.75, 2.48, "Sequencer",
                   "実行順・再開位置を管理", CYAN, 26, 19)
        self.play(LaggedStart(*[FadeIn(r, shift=LEFT*.16)
                                for r in rows], lag_ratio=.15),
                  FadeIn(hub, shift=RIGHT*.18), run_time=1.12)
        connectors = [
            link([-.2,1.15,0],[1.77,.65,0],CYAN,3.4),
            link([-.2,-.12,0],[1.77,-.12,0],BLUE,3.4),
            link([-.2,-1.39,0],[1.77,-.86,0],PURPLE,3.4),
        ]
        self.play(*[GrowArrow(a) for a in connectors], run_time=.68)
        self.play(Indicate(rows[0][0], color=ORANGE),
                  Indicate(hub[0], color=CYAN), run_time=.82)
        status = txt("スタックフルFiberなら、深い呼び出し先からもyield可能", 20, YELLOW)
        status.move_to([0, -2.50, 0])
        self.play(FadeIn(status), run_time=.42)
        self.footer(4, "非warpループやwaitは他スレッドへ制御を譲る。warpにも時間予算がある。")
        self.wait(2.4)

    def safety(self):
        self.heading(5, "速さより、まず意味論", "数値特化が安全でない場合は汎用処理を使う")
        source = card(-4.72, 0.25, 2.84, 1.36, "Scratch Value",
                      "数値 / 文字列 / NaN…", BLUE, 23)
        guard = card(-.36, .25, 2.60, 1.36, "型ガード",
                     "条件をチェック", YELLOW, 25)
        fast = card(4.22, 1.06, 3.25, 1.23, "Fast Path",
                    "最適化された数値計算", GREEN, 23, 17)
        slow = card(4.22, -1.04, 3.25, 1.23, "Runtime / Interp",
                    "Scratch互換の処理", PINK, 23, 17)
        self.play(FadeIn(source), FadeIn(guard), run_time=.60)
        self.play(GrowArrow(link([-3.2,.25,0],[-1.72,.25,0],BLUE)),run_time=.43)
        self.play(FadeIn(fast), FadeIn(slow), run_time=.62)
        branches=[
            link([1.01,.45,0],[2.56,1.04,0],GREEN),
            link([1.01,.04,0],[2.56,-1.02,0],PINK),
        ]
        self.play(GrowArrow(branches[0]), GrowArrow(branches[1]),run_time=.53)
        yes=txt("数値が保証できる", 18, GREEN).move_to([1.62,1.54,0])
        no=txt("型が変わる・未対応", 18, PINK).move_to([1.52,-1.64,0])
        self.play(FadeIn(yes), FadeIn(no),run_time=.35)
        status = txt("無理にJITせず、正しい意味の処理に戻せることが重要", 21, YELLOW)
        status.move_to([0,-2.46,0])
        self.play(FadeIn(status), run_time=.36)
        self.footer(5, "型が変わるScratchだからこそ、ガードとフォールバックを設計する。")
        self.wait(2.2)

    def experimental(self):
        self.heading(6, "実験機能：非同期JITとOSR",
                     "コンパイル中もInterpreterを続け、条件が合えば途中から昇格")
        begin = card(-4.82, .95, 2.75, 1.18, "Interpreter",
                     "すぐ実行開始", ORANGE, 23)
        boundary = card(-.62, .95, 2.95, 1.18, "安全なループ境界",
                        "状態を引き継ぐ", YELLOW, 21, 17)
        native = card(4.35, .95, 3.00, 1.18, "LLVM Native",
                      "残りを実行", CYAN, 24)
        self.play(FadeIn(begin), FadeIn(boundary), FadeIn(native),run_time=.80)
        self.play(GrowArrow(link([-3.38,.95,0],[-2.20,.95,0],ORANGE)),
                  GrowArrow(link([.90,.95,0],[2.78,.95,0],CYAN)),run_time=.58)
        worker = card(-.30, -1.01, 7.1, 1.12, "バックグラウンドで LLVM コンパイル",
                      "IR生成後、O3最適化と機械語生成をワーカーで進める", PURPLE, 22, 17)
        self.play(FadeIn(worker, shift=DOWN*.2),run_time=.56)
        dashed = Line([-.62,-.44,0],[-.62,.34,0],color=PURPLE,
                      stroke_width=2).set_opacity(.85)
        self.play(Create(dashed), Indicate(boundary[0],color=YELLOW),run_time=.65)
        markers=VGroup(
            badge("opt-in：XYO_JIT_ASYNC=1", -3.08, -2.24, ORANGE, 4.1),
            badge("opt-in：XYO_JIT_OSR=1",  2.34, -2.24, YELLOW, 4.1)
        )
        self.play(FadeIn(markers),run_time=.45)
        self.footer(6, "OSRは条件を満たすトップレベルrepeatなどに限定。汎用OSRではない。")
        self.wait(2.25)

    def backends(self):
        self.heading(7, "実行先に応じた3つの経路",
                     "LLVM・WebAssembly・AOTは同じ機能範囲ではない")
        source=card(0,1.40,4.80,1.10,"Scratchスクリプト",
                    "共通の構造解析と、個別のコード生成",BLUE,25,17)
        self.play(FadeIn(source,shift=DOWN*.12),run_time=.57)
        branches=[
            card(-4.55,-.59,3.70,1.86,"LLVM JIT",
                 "ネイティブ機械語\nLinux / Windows / macOS",CYAN,25,18),
            card(0,-.59,3.70,1.86,"WASM",
                 "WebAssembly + Binaryen\n対応する数値系を高速化",PURPLE,25,17),
            card(4.55,-.59,3.70,1.86,"AOT",
                 "事前にコンパイル\n実行ファイルを書き出す",ORANGE,25,18),
        ]
        self.play(LaggedStart(*[FadeIn(b,shift=UP*.15) for b in branches],
                                lag_ratio=.16),run_time=.98)
        arrows=[
            link([-1.35,.83,0],[-4.15,.43,0],CYAN),
            link([0,.82,0],[0,.42,0],PURPLE),
            link([1.35,.83,0],[4.15,.43,0],ORANGE),
        ]
        self.play(*[GrowArrow(a) for a in arrows],run_time=.62)
        note=txt("WASM JITは限定対応。未対応ブロックはInterpreterが実行",20,YELLOW)
        note.move_to([0,-2.32,0])
        self.play(FadeIn(note),run_time=.35)
        self.footer(7, "使い分けは「いつ」「どのコードを」「どこへ」コンパイルするか。")
        self.wait(2.3)

    def outro(self):
        self.clear_page()
        title=txt("正確さを維持しながら、",39,WHITE,True)
        title2=txt("熱い部分だけ高速化する。",43,CYAN,True)
        title.move_to([0,2.45,0])
        title2.move_to([0,1.65,0])
        self.play(FadeIn(title,shift=UP*.12),FadeIn(title2,shift=UP*.12),
                  run_time=.9)
        choices=[
            badge("互換性  :  Interpreter",0,.55,ORANGE,6.8),
            badge("高速化  :  LLVM HotSpot JIT",0,-.26,CYAN,6.8),
            badge("配布  :  AOT",0,-1.07,PURPLE,6.8),
        ]
        self.play(LaggedStart(*[FadeIn(b,shift=UP*.13) for b in choices],
                                lag_ratio=.2),run_time=1.14)
        footer=txt("xyo-cpp  •  実装資料ベース / 2026年11月",21,MUTED)
        footer.move_to([0,-2.47,0])
        self.play(FadeIn(footer),run_time=.36)
        self.wait(2.8)
