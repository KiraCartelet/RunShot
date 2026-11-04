#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import os
import re
import shlex
import shutil
import signal
import subprocess
import tempfile

import gi
gi.require_version("Gtk", "3.0")
from gi.repository import Gtk, GLib

# init.sh / save.sh(.bak) 模板里必须保留这个占位符
PROGRAM_PLACEHOLDER = "./example"

# recover.sh 里的 Chipyard 仿真器配置映射；QEMU 不走 recover.sh，而是直接运行 recover_qemu
SIMULATOR_CONFIG_MAP = {
    "Rocket": "RocketConfig",
    "BOOM": "MediumBoomV3Config",
}

QEMU_SIMULATOR_KEY = "QEMU"
QEMU_RECOVER_CANDIDATES = ("recover_qemu", "recover_qemu.sh")

RET_RE = re.compile(r"\bret\b")


class MainWindow(Gtk.Window):
    def __init__(self):
        super().__init__(title="QEMU Plugin Control")
        self.set_default_size(1100, 760)
        self.set_border_width(12)

        self.workdir = os.path.dirname(os.path.abspath(__file__))

        # 运行状态
        self.proc = None
        self.proc_kind = None          # "init" / "save" / "recover" / "recover_qemu"
        self.watch_id = None
        self.stop_pending_kill = False
        self.pending_program_path = None

        # assembly view state
        self.asm_lines = []
        self.ret_line_indices = []
        self.highlight_tag = None
        self.last_highlight_range = None
        self.ret_mark = None

        root = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10)
        self.add(root)

        # ====== Row 1: Program chooser ======
        row_prog = Gtk.Box(spacing=10)
        root.pack_start(row_prog, False, False, 0)

        row_prog.pack_start(Gtk.Label(label="Program =", xalign=0), False, False, 0)

        self.prog_chooser = Gtk.FileChooserButton(
            title="选择要运行的目标程序",
            action=Gtk.FileChooserAction.OPEN
        )
        self.prog_chooser.set_hexpand(True)
        row_prog.pack_start(self.prog_chooser, True, True, 0)

        default_prog = os.path.join(self.workdir, "example")
        if os.path.exists(default_prog):
            self.prog_chooser.set_filename(default_prog)
        self.prog_chooser.connect("file-set", self.on_program_changed)

        # ====== Row 2: Simulator + k ======
        row_ctrl = Gtk.Box(spacing=14)
        root.pack_start(row_ctrl, False, False, 0)

        row_ctrl.pack_start(Gtk.Label(label="Simulator =", xalign=0), False, False, 0)

        self.sim_combo = Gtk.ComboBoxText()
        self.sim_combo.append("Rocket", "Rocket")
        self.sim_combo.append("BOOM", "BOOM")
        self.sim_combo.append(QEMU_SIMULATOR_KEY, "QEMU")
        self.sim_combo.set_active_id("Rocket")
        self.sim_combo.connect("changed", self.on_simulator_changed)
        row_ctrl.pack_start(self.sim_combo, False, False, 0)

        self.lbl_sim_info = Gtk.Label(label=self.get_simulator_info_text(), xalign=0)
        row_ctrl.pack_start(self.lbl_sim_info, False, False, 0)

        row_ctrl.pack_start(Gtk.Label(label="k =", xalign=0), False, False, 16)

        self.k_adj = Gtk.Adjustment(value=1, lower=1, upper=1, step_increment=1, page_increment=10)
        self.spin_k = Gtk.SpinButton(adjustment=self.k_adj, climb_rate=1, digits=0)
        self.spin_k.set_numeric(True)
        self.spin_k.connect("value-changed", self.on_k_changed)
        row_ctrl.pack_start(self.spin_k, False, False, 0)

        self.lbl_k_info = Gtk.Label(label="ret: ?   k_max: ?", xalign=0)
        row_ctrl.pack_start(self.lbl_k_info, False, False, 0)

        # ====== Row 3: Buttons ======
        row_btn = Gtk.Box(spacing=14)
        root.pack_start(row_btn, False, False, 0)

        self.btn_save = Gtk.Button(label="Run save.sh (保存断点数据)")
        self.btn_recover = Gtk.Button(label="Run recover (恢复并运行)")
        self.btn_stop = Gtk.Button(label="Stop (停止)")
        self.btn_stop.set_sensitive(False)

        self.btn_save.connect("clicked", self.on_run_save)
        self.btn_recover.connect("clicked", self.on_run_recover)
        self.btn_stop.connect("clicked", self.on_stop)

        row_btn.pack_start(self.btn_save, False, False, 0)
        row_btn.pack_start(self.btn_recover, False, False, 0)
        row_btn.pack_start(self.btn_stop, False, False, 0)

        # ====== Notebook: Output / Assembly ======
        nb = Gtk.Notebook()
        root.pack_start(nb, True, True, 0)

        # Output page
        out_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
        nb.append_page(out_box, Gtk.Label(label="Output"))

        self.out_buf = Gtk.TextBuffer()
        self.out_view = Gtk.TextView(buffer=self.out_buf)
        self.out_view.set_editable(False)
        self.out_view.set_monospace(True)
        out_sc = Gtk.ScrolledWindow()
        out_sc.add(self.out_view)
        out_box.pack_start(out_sc, True, True, 0)

        # Assembly page
        asm_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
        nb.append_page(asm_box, Gtk.Label(label="Assembly"))

        self.asm_buf = Gtk.TextBuffer()
        self.highlight_tag = self.asm_buf.create_tag("hl", background="#FFF2A8")
        self.asm_view = Gtk.TextView(buffer=self.asm_buf)
        self.asm_view.set_editable(False)
        self.asm_view.set_monospace(True)
        asm_sc = Gtk.ScrolledWindow()
        asm_sc.add(self.asm_view)
        asm_box.pack_start(asm_sc, True, True, 0)

        # ====== Start ======
        self._ensure_scripts_program_patched()
        self._ensure_recover_simulator_patched()
        self._start_init()

    # ---------------- helpers ----------------
    def log(self, s: str):
        it = self.out_buf.get_end_iter()
        self.out_buf.insert(it, s)
        vadj = self.out_view.get_parent().get_vadjustment()
        GLib.idle_add(lambda: vadj.set_value(vadj.get_upper() - vadj.get_page_size()) or False)

    def get_program_path(self) -> str:
        p = self.prog_chooser.get_filename()
        return os.path.abspath(p) if p else ""

    def get_simulator_key(self) -> str:
        sim_key = self.sim_combo.get_active_id()
        return sim_key if sim_key else "Rocket"

    def is_qemu_selected(self) -> bool:
        return self.get_simulator_key() == QEMU_SIMULATOR_KEY

    def get_simulator_config(self) -> str:
        sim_key = self.get_simulator_key()
        return SIMULATOR_CONFIG_MAP.get(sim_key, "RocketConfig")

    def get_simulator_info_text(self) -> str:
        if self.is_qemu_selected():
            return "script: recover_qemu"
        return f"config: {self.get_simulator_config()}"

    def get_recover_script_path(self) -> str:
        if self.is_qemu_selected():
            for name in QEMU_RECOVER_CANDIDATES:
                path = os.path.join(self.workdir, name)
                if os.path.exists(path):
                    return path
            candidates = " / ".join(os.path.join(self.workdir, x) for x in QEMU_RECOVER_CANDIDATES)
            raise FileNotFoundError(f"找不到 QEMU 恢复脚本：{candidates}")

        return os.path.join(self.workdir, "recover.sh")

    def _set_buttons_running(self, running: bool):
        if running:
            self.btn_save.set_sensitive(False)
            self.btn_recover.set_sensitive(False)
            self.btn_stop.set_sensitive(True)
        else:
            self.btn_save.set_sensitive(True)
            self.btn_recover.set_sensitive(True)
            self.btn_stop.set_sensitive(False)

    # ---------------- script patching: init/save program ----------------
    def _patch_program_in_script_from_bak(self, script_name: str, program_path: str):
        """
        永远从 .bak 模板生成脚本。
        模板里必须保留 ./example，占位符会被全局替换，
        因此能同时替换 elf=./example 和最后那个 ./example。
        """
        script_path = os.path.join(self.workdir, script_name)
        if not os.path.exists(script_path):
            raise FileNotFoundError(f"找不到 {script_path}")

        bak_path = script_path + ".bak"
        if not os.path.exists(bak_path):
            shutil.copy2(script_path, bak_path)

        with open(bak_path, "r", encoding="utf-8") as f:
            template = f.read()

        if PROGRAM_PLACEHOLDER not in template:
            raise RuntimeError(
                f"{script_name}.bak 中未找到占位符 {PROGRAM_PLACEHOLDER!r}。"
                f"请把 {script_name} / {script_name}.bak 恢复成含占位符的版本，"
                f"或删除 .bak 后重新运行程序。"
            )

        quoted = shlex.quote(os.path.abspath(program_path))
        new_text = template.replace(PROGRAM_PLACEHOLDER, quoted)

        fd, tmp = tempfile.mkstemp(prefix=script_name + ".", suffix=".tmp", dir=self.workdir)
        os.close(fd)
        with open(tmp, "w", encoding="utf-8") as f:
            f.write(new_text)
        os.replace(tmp, script_path)

    def _ensure_scripts_program_patched(self) -> bool:
        """
        只改 init.sh / save.sh 里的 Program，不改 recover.sh
        """
        program_path = self.get_program_path()
        if not program_path:
            self.log("[UI][WARN] 还没有选择 Program，init/save 的程序路径暂不修改。\n")
            return False
        if not os.path.exists(program_path):
            self.log(f"[UI][ERROR] Program 不存在：{program_path}\n")
            return False

        try:
            for s in ("init.sh", "save.sh"):
                self._patch_program_in_script_from_bak(s, program_path)
            self.log(f"[UI] 已写入 init/save 的 Program：{program_path}\n")
            return True
        except Exception as e:
            self.log(f"[UI][ERROR] 修改脚本 Program 失败：{e}\n")
            return False

    # ---------------- script patching: recover simulator ----------------
    def _patch_recover_sh_simulator_from_bak(self, sim_config: str):
        """
        从 recover.sh.bak 生成 recover.sh，并替换：
        simulator-chipyard.harness-XXX-debug
        里的 XXX 为 sim_config
        """
        script_path = os.path.join(self.workdir, "recover.sh")
        if not os.path.exists(script_path):
            raise FileNotFoundError(f"找不到 {script_path}")

        bak_path = script_path + ".bak"
        if not os.path.exists(bak_path):
            shutil.copy2(script_path, bak_path)

        with open(bak_path, "r", encoding="utf-8") as f:
            template = f.read()

        pattern = r"(simulator-chipyard\.harness-)([^/\s]+?)(-debug\b)"

        def repl(m):
            return f"{m.group(1)}{sim_config}{m.group(3)}"

        new_text, n = re.subn(pattern, repl, template, count=1)
        if n == 0:
            raise RuntimeError(
                "没有在 recover.sh.bak 中找到 "
                "`simulator-chipyard.harness-XXX-debug` 这段，无法替换仿真器。"
            )

        fd, tmp = tempfile.mkstemp(prefix="recover.", suffix=".sh", dir=self.workdir)
        os.close(fd)
        with open(tmp, "w", encoding="utf-8") as f:
            f.write(new_text)
        os.replace(tmp, script_path)

    def _ensure_recover_simulator_patched(self) -> bool:
        self.lbl_sim_info.set_text(self.get_simulator_info_text())

        # QEMU 模式不需要改 recover.sh，直接在运行时执行 recover_qemu
        if self.is_qemu_selected():
            try:
                qemu_path = self.get_recover_script_path()
                self.log(f"[UI] QEMU 模式：将运行 {qemu_path}\n")
                return True
            except Exception as e:
                self.log(f"[UI][ERROR] {e}\n")
                return False

        sim_config = self.get_simulator_config()

        try:
            self._patch_recover_sh_simulator_from_bak(sim_config)
            self.log(f"[UI] 已写入 recover 的仿真器：{sim_config}\n")
            return True
        except Exception as e:
            self.log(f"[UI][ERROR] 修改 recover.sh 仿真器失败：{e}\n")
            return False

    # ---------------- init / run process ----------------
    def _start_init(self):
        if self.proc is not None:
            self.log("[UI] 有进程在运行，无法启动 init。\n")
            return

        init_path = os.path.join(self.workdir, "init.sh")
        if not os.path.exists(init_path):
            self.log(f"[UI][ERROR] 找不到 {init_path}\n")
            return

        self.log("[UI] Running init.sh ...\n")
        self._start_process(kind="init", cmd=["bash", init_path])

    def _start_process(self, kind: str, cmd):
        self.proc_kind = kind
        self._set_buttons_running(True)
        self.stop_pending_kill = False

        self.log("[UI] exec: " + " ".join(shlex.quote(x) for x in cmd) + "\n")
        self.log("----------------------------------------\n")

        self.proc = subprocess.Popen(
            cmd,
            cwd=self.workdir,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
            preexec_fn=os.setsid,
        )

        fd = self.proc.stdout.fileno()
        self.watch_id = GLib.io_add_watch(fd, GLib.IO_IN | GLib.IO_HUP, self._on_proc_io)

    def _on_proc_io(self, _source, _cond):
        if self.proc is None or self.proc.stdout is None:
            return False

        line = self.proc.stdout.readline()
        if line:
            self.log(line)
            return True

        ret = self.proc.poll()
        if ret is not None:
            self.log("----------------------------------------\n")
            self.log(f"[UI] Finished ({self.proc_kind}). exit code = {ret}\n")
            kind = self.proc_kind
            self._cleanup_proc()

            if kind == "init" and ret == 0:
                self._load_assembly_and_index()

                if self.pending_program_path:
                    self.pending_program_path = None
                    if self._ensure_scripts_program_patched():
                        self._start_init()
            return False

        return True

    def _cleanup_proc(self):
        if self.watch_id is not None:
            try:
                GLib.source_remove(self.watch_id)
            except Exception:
                pass
            self.watch_id = None

        if self.proc is not None:
            try:
                if self.proc.stdout:
                    self.proc.stdout.close()
            except Exception:
                pass

        self.proc = None
        self.proc_kind = None
        self._set_buttons_running(False)

    def on_stop(self, _btn):
        if self.proc is None:
            return

        self.log("[UI] Stopping...\n")
        try:
            os.killpg(self.proc.pid, signal.SIGTERM)
        except Exception as e:
            self.log(f"[UI][ERROR] terminate failed: {e}\n")
            return

        if not self.stop_pending_kill:
            self.stop_pending_kill = True
            GLib.timeout_add(1200, self._maybe_force_kill)

    def _maybe_force_kill(self):
        if self.proc is None:
            return False
        if self.proc.poll() is not None:
            return False

        self.log("[UI] Still running, force kill...\n")
        try:
            os.killpg(self.proc.pid, signal.SIGKILL)
        except Exception as e:
            self.log(f"[UI][ERROR] kill failed: {e}\n")
        return False

    # ---------------- UI events ----------------
    def on_program_changed(self, _btn):
        if self.proc is not None:
            if self.proc_kind == "init":
                self.pending_program_path = self.get_program_path()
                self.log("[UI] init.sh 正在运行，已记录 Program 切换，待 init 完成后自动应用并重新 init。\n")
                return
            else:
                self.log("[UI] 当前有脚本在运行，先 Stop 再切换 Program。\n")
                return

        ok = self._ensure_scripts_program_patched()
        if ok:
            self._start_init()

    def on_simulator_changed(self, _combo):
        self.lbl_sim_info.set_text(self.get_simulator_info_text())

        if self.proc is not None:
            self.log("[UI] 当前有脚本在运行，新的仿真器选择会在下次 recover 前应用。\n")
            return

        self._ensure_recover_simulator_patched()

    def on_run_save(self, _btn):
        if self.proc is not None:
            self.log("[UI] 有进程在运行，先 Stop 或等待结束。\n")
            return

        if not self._ensure_scripts_program_patched():
            return

        k = int(self.spin_k.get_value())
        kmax = int(self.k_adj.get_upper())

        if kmax < 1:
            self.log("[UI][ERROR] 当前没有可用的 k（ret 数量不足或 assembly.data 异常）。\n")
            return

        if k < 1:
            k = 1
        if k > kmax:
            k = kmax

        self.spin_k.set_value(k)

        try:
            self._patch_save_sh_k(k)
            self.log(f"[UI] 已写入 save.sh：k={k}\n")
        except Exception as e:
            self.log(f"[UI][ERROR] 修改 save.sh 失败：{e}\n")
            return

        save_path = os.path.join(self.workdir, "save.sh")
        self._start_process(kind="save", cmd=["bash", save_path])

    def on_run_recover(self, _btn):
        if self.proc is not None:
            self.log("[UI] 有进程在运行，先 Stop 或等待结束。\n")
            return

        if not self._ensure_recover_simulator_patched():
            return

        try:
            recover_path = self.get_recover_script_path()
        except Exception as e:
            self.log(f"[UI][ERROR] {e}\n")
            return

        if not os.path.exists(recover_path):
            self.log(f"[UI][ERROR] 找不到 {recover_path}\n")
            return

        if self.is_qemu_selected():
            self._start_process(kind="recover_qemu", cmd=["bash", recover_path])
        else:
            self._start_process(kind="recover", cmd=["bash", recover_path])

    def on_k_changed(self, _spin):
        k = int(self.spin_k.get_value())
        self._jump_to_kth_ret(k)

    # ---------------- save.sh patch k ----------------
    def _patch_save_sh_k(self, k: int):
        save_path = os.path.join(self.workdir, "save.sh")
        if not os.path.exists(save_path):
            raise FileNotFoundError(f"找不到 {save_path}")

        with open(save_path, "r", encoding="utf-8") as f:
            text = f.read()

        pattern = r"(-plugin\s+\S*libsnapshot\.so[^ \t\r\n]*?)\bk=\d+"
        new_text, n = re.subn(pattern, rf"\1k={k}", text)
        if n == 0:
            raise RuntimeError("没有在 save.sh 中找到包含 libsnapshot.so 的 `k=数字`，无法替换。")

        fd, tmp = tempfile.mkstemp(prefix="save.", suffix=".sh", dir=self.workdir)
        os.close(fd)
        with open(tmp, "w", encoding="utf-8") as f:
            f.write(new_text)
        os.replace(tmp, save_path)

    # ---------------- Assembly loading + indexing ----------------
    def _load_assembly_and_index(self):
        asm_path = os.path.join(self.workdir, "assembly.data")
        if not os.path.exists(asm_path):
            self.log(f"[UI][ERROR] 找不到 {asm_path}\n")
            return

        try:
            with open(asm_path, "r", encoding="utf-8", errors="ignore") as f:
                lines = f.read().splitlines()

            last_num = None
            if lines and lines[-1].strip().isdigit():
                last_num = int(lines[-1].strip())
                lines = lines[:-1]

            self.asm_lines = lines

            # set_text 会让旧 TextIter 失效，必须先丢弃旧高亮状态
            self.last_highlight_range = None
            if self.ret_mark is not None:
                try:
                    self.asm_buf.delete_mark(self.ret_mark)
                except Exception:
                    pass
                self.ret_mark = None

            self.asm_buf.set_text("\n".join(lines) + ("\n" if lines else ""))

            self.ret_line_indices = [i for i, line in enumerate(lines) if RET_RE.search(line)]
            ret_cnt = len(self.ret_line_indices)

            k_max = ret_cnt

            if last_num is not None:
                last_num_1b = last_num + 1 if last_num == ret_cnt - 1 else last_num

                if last_num_1b != ret_cnt:
                    self.log(
                        f"[UI][WARN] assembly.data 末尾数字={last_num}（视作1b={last_num_1b}）"
                        f"与 ret_count={ret_cnt} 不一致，采用更安全的 min。\n"
                    )

                k_max = min(ret_cnt, last_num_1b)

            if k_max <= 0:
                self.k_adj.set_lower(0)
                self.k_adj.set_upper(0)
                self.spin_k.set_value(0)
                self.lbl_k_info.set_text(f"ret: {ret_cnt}   k_max: 0")
                self.log("[UI][WARN] 没有找到可用的 ret，无法设置有效的 k。\n")
                return

            self.k_adj.set_lower(1)
            self.k_adj.set_upper(k_max)
            self.lbl_k_info.set_text(f"ret: {ret_cnt}   k_max: {k_max}")

            cur_k = int(self.spin_k.get_value())
            if cur_k < 1:
                cur_k = 1
            if cur_k > k_max:
                cur_k = k_max

            self.spin_k.set_value(cur_k)
            self._jump_to_kth_ret(cur_k)

            self.log("[UI] Assembly loaded. Indexed ret instructions.\n")

        except Exception as e:
            self.log(f"[UI][ERROR] 读取/索引 assembly.data 失败：{e}\n")

    def _clear_last_highlight(self):
        if self.last_highlight_range is None:
            return

        start, end = self.last_highlight_range
        self.last_highlight_range = None

        try:
            self.asm_buf.remove_tag(self.highlight_tag, start, end)
        except Exception:
            pass

    def _jump_to_kth_ret(self, k: int):
        total_lines = len(self.asm_lines)
        if total_lines == 0:
            return
        if not self.ret_line_indices:
            return

        ret_cnt = len(self.ret_line_indices)

        if k < 1:
            k = 1
        if k > ret_cnt:
            k = ret_cnt

        ret_line = self.ret_line_indices[k - 1]
        target_line = ret_line + 1
        if target_line >= total_lines:
            target_line = total_lines - 1

        start = self.asm_buf.get_iter_at_line(target_line)
        end = start.copy()
        if not end.ends_line():
            end.forward_to_line_end()
        end2 = end.copy()
        if end2.forward_char():
            end = end2

        self._clear_last_highlight()
        self.asm_buf.apply_tag(self.highlight_tag, start, end)
        self.last_highlight_range = (start.copy(), end.copy())

        if self.ret_mark is None:
            self.ret_mark = self.asm_buf.create_mark("ret_mark", start, True)
        else:
            self.asm_buf.move_mark(self.ret_mark, start)

        self.asm_view.scroll_to_mark(self.ret_mark, 0.2, True, 0.0, 0.2)


def main():
    win = MainWindow()
    win.connect("destroy", Gtk.main_quit)
    win.show_all()
    Gtk.main()


if __name__ == "__main__":
    main()
