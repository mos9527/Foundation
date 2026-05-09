# /// script
# requires-python = ">=3.11,<3.12"
# dependencies = [
#   "numpy",
#   "opencolorio",
# ]
# ///

from __future__ import annotations

import ctypes
import sys
import threading
import time
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

from OCIOBakeLUTs import (
    DEFAULT_DISPLAY,
    DEFAULT_INPUT_SPACE,
    DEFAULT_LUT_SIZE,
    bake_lut_rgba,
    create_config,
    create_processor,
    write_lut_dds,
    write_lut_tga,
)

DEFAULT_VIEW = "ACES 1.3"
NO_LOOK = "No Look"


def enable_hidpi() -> None:
    if sys.platform != "win32":
        return

    try:
        ctypes.windll.user32.SetProcessDpiAwarenessContext(-4)  # PER_MONITOR_AWARE_V2
        return
    except Exception:
        pass

    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)  # PROCESS_PER_MONITOR_DPI_AWARE
        return
    except Exception:
        pass

    try:
        ctypes.windll.user32.SetProcessDPIAware()
    except Exception:
        pass


def _split_look_names(value: str) -> list[str]:
    names: list[str] = []
    for part in value.split(","):
        name = part.strip().lstrip("+")
        if name:
            names.append(name)
    return names


def _infer_blender_view_looks(view: str, look_values: list[str]) -> list[str]:
    if view.startswith("ACES 1.3"):
        return [look for look in look_values if look.startswith("ACES 1.3")]
    if view.startswith("ACES 2.0"):
        return [look for look in look_values if look.startswith("ACES 2.0")]
    if view.startswith("AgX"):
        return [look for look in look_values if look.startswith("AgX")]
    if view.startswith("Filmic"):
        filmic_looks = [look for look in look_values if "Filmic" in look]
        if filmic_looks:
            return filmic_looks
        return [look for look in look_values if not look.startswith("AgX") and not look.startswith("ACES ")]
    if view in {"Standard", "Khronos PBR Neutral", "False Color", "Raw"}:
        return []
    return look_values


class OCIOBakeLUTsGui:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("OCIO Bake LUTs")
        self.root.minsize(720, 440)

        self.config = None
        self.display_values: list[str] = []
        self.view_values: list[str] = []
        self.look_values: list[str] = []

        self.config_var = tk.StringVar()
        self.output_var = tk.StringVar(value=str(Path.cwd() / "view_lut.tga"))
        self.display_var = tk.StringVar()
        self.view_var = tk.StringVar()
        self.look_var = tk.StringVar()
        self.input_space_var = tk.StringVar(value=DEFAULT_INPUT_SPACE)
        self.size_var = tk.StringVar(value=str(DEFAULT_LUT_SIZE))
        self.format_var = tk.StringVar(value="tga")
        self.flip_u_var = tk.BooleanVar(value=False)
        self.flip_v_var = tk.BooleanVar(value=True)
        self.status_var = tk.StringVar(value="Ready")

        self._build_ui()
        self._refresh_format_state()

    def _build_ui(self) -> None:
        outer = ttk.Frame(self.root, padding=12)
        outer.grid(row=0, column=0, sticky="nsew")
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(0, weight=1)
        outer.columnconfigure(1, weight=1)

        row = 0
        ttk.Label(outer, text="Config").grid(row=row, column=0, sticky="w", padx=(0, 8), pady=4)
        self.config_entry = ttk.Entry(outer, textvariable=self.config_var)
        self.config_entry.grid(row=row, column=1, sticky="ew", pady=4)
        self.config_entry.bind("<Return>", lambda _event: self.load_config(show_success=True))
        ttk.Button(outer, text="Browse...", command=self.browse_config).grid(row=row, column=2, padx=(8, 0), pady=4)

        row += 1
        ttk.Label(outer, text="Output").grid(row=row, column=0, sticky="w", padx=(0, 8), pady=4)
        ttk.Entry(outer, textvariable=self.output_var).grid(row=row, column=1, sticky="ew", pady=4)
        ttk.Button(outer, text="Browse...", command=self.browse_output).grid(row=row, column=2, padx=(8, 0), pady=4)

        row += 1
        ttk.Label(outer, text="Display").grid(row=row, column=0, sticky="w", padx=(0, 8), pady=4)
        self.display_combo = ttk.Combobox(outer, textvariable=self.display_var, values=self.display_values)
        self.display_combo.grid(row=row, column=1, sticky="ew", pady=4)
        self.display_combo.bind("<<ComboboxSelected>>", lambda _event: self.refresh_views())

        row += 1
        ttk.Label(outer, text="View").grid(row=row, column=0, sticky="w", padx=(0, 8), pady=4)
        self.view_combo = ttk.Combobox(outer, textvariable=self.view_var, values=self.view_values)
        self.view_combo.grid(row=row, column=1, sticky="ew", pady=4)
        self.view_combo.bind("<<ComboboxSelected>>", lambda _event: self.refresh_looks())

        row += 1
        ttk.Label(outer, text="Look").grid(row=row, column=0, sticky="w", padx=(0, 8), pady=4)
        self.look_combo = ttk.Combobox(outer, textvariable=self.look_var, values=[])
        self.look_combo.grid(row=row, column=1, sticky="ew", pady=4)

        row += 1
        ttk.Label(outer, text="Input Space").grid(row=row, column=0, sticky="w", padx=(0, 8), pady=4)
        ttk.Entry(outer, textvariable=self.input_space_var).grid(row=row, column=1, sticky="ew", pady=4)

        row += 1
        ttk.Label(outer, text="Size").grid(row=row, column=0, sticky="w", padx=(0, 8), pady=4)
        ttk.Spinbox(outer, textvariable=self.size_var, from_=2, to=256, increment=1, width=8).grid(row=row, column=1, sticky="w", pady=4)

        row += 1
        format_frame = ttk.LabelFrame(outer, text="Output Format", padding=8)
        format_frame.grid(row=row, column=0, columnspan=4, sticky="ew", pady=(10, 4))
        format_frame.columnconfigure(4, weight=1)
        ttk.Radiobutton(format_frame, text="TGA tiled (Unity default)", value="tga", variable=self.format_var, command=self._refresh_format_state).grid(row=0, column=0, sticky="w")
        ttk.Radiobutton(format_frame, text="DDS 3D", value="dds", variable=self.format_var, command=self._refresh_format_state).grid(row=0, column=1, sticky="w", padx=(16, 0))
        self.flip_u_check = ttk.Checkbutton(format_frame, text="Flip U", variable=self.flip_u_var)
        self.flip_u_check.grid(row=0, column=2, sticky="w", padx=(24, 0))
        self.flip_v_check = ttk.Checkbutton(format_frame, text="Flip V", variable=self.flip_v_var)
        self.flip_v_check.grid(row=0, column=3, sticky="w", padx=(12, 0))

        row += 1
        action_frame = ttk.Frame(outer)
        action_frame.grid(row=row, column=0, columnspan=4, sticky="ew", pady=(10, 4))
        action_frame.columnconfigure(1, weight=1)
        self.bake_button = ttk.Button(action_frame, text="Bake", command=self.bake)
        self.bake_button.grid(row=0, column=0, sticky="w")
        ttk.Label(action_frame, textvariable=self.status_var).grid(row=0, column=1, sticky="w", padx=(12, 0))

        row += 1
        self.log = tk.Text(outer, height=8, wrap="word")
        self.log.grid(row=row, column=0, columnspan=4, sticky="nsew", pady=(8, 0))
        outer.rowconfigure(row, weight=1)

    def browse_config(self) -> None:
        path = filedialog.askopenfilename(
            title="Select OCIO config",
            filetypes=[("OCIO config", "*.ocio"), ("All files", "*.*")],
        )
        if path:
            self.config_var.set(path)
            self.load_config(show_success=True)

    def browse_output(self) -> None:
        extension = ".tga" if self.format_var.get() == "tga" else ".dds"
        path = filedialog.asksaveasfilename(
            title="Save LUT",
            defaultextension=extension,
            filetypes=[("TGA", "*.tga"), ("DDS", "*.dds"), ("All files", "*.*")],
            initialfile=f"view_lut{extension}",
        )
        if path:
            self.output_var.set(path)

    def load_config(self, *, show_success: bool) -> None:
        path_text = self.config_var.get().strip()
        if not path_text:
            self._set_status("Select an OCIO config first.")
            return

        path = Path(path_text)
        try:
            config = create_config(path)
        except Exception as exc:
            self._set_status(f"Failed to load config: {exc}")
            messagebox.showerror("Load Config", str(exc))
            return

        self.config = config
        self.display_values = list(config.getDisplays())
        self.look_values = [look.getName() for look in config.getLooks()]
        self.display_combo.configure(values=self.display_values)

        if self.display_values and self.display_var.get() not in self.display_values:
            self.display_var.set(DEFAULT_DISPLAY if DEFAULT_DISPLAY in self.display_values else self.display_values[0])

        self.refresh_views()
        self._set_status(f"Loaded config: {path}")
        if show_success:
            self._log(f"Loaded config: {path}")

    def refresh_views(self) -> None:
        if self.config is None:
            return

        display = self.display_var.get()
        self.view_values = list(self.config.getViews(display))
        self.view_combo.configure(values=self.view_values)

        if self.view_values and self.view_var.get() not in self.view_values:
            self.view_var.set(DEFAULT_VIEW if DEFAULT_VIEW in self.view_values else self.view_values[0])

        self.refresh_looks()

    def refresh_looks(self) -> None:
        display = self.display_var.get()
        view = self.view_var.get()
        look_values = self.view_looks(display, view)
        self.look_combo.configure(values=[NO_LOOK, *look_values])

        if self.look_var.get() not in [NO_LOOK, *look_values]:
            self.look_var.set(NO_LOOK)

    def view_looks(self, display: str, view: str) -> list[str]:
        if self.config is None:
            return []

        getter = getattr(self.config, "getDisplayViewLooks", None)
        if getter is not None:
            try:
                declared_looks = _split_look_names(getter(display, view))
            except Exception:
                declared_looks = []
            if declared_looks:
                available = set(self.look_values)
                return [look for look in declared_looks if look in available]

        return _infer_blender_view_looks(view, self.look_values)

    def _refresh_format_state(self) -> None:
        is_tga = self.format_var.get() == "tga"
        state = "normal" if is_tga else "disabled"
        self.flip_u_check.configure(state=state)
        self.flip_v_check.configure(state=state)

    def bake(self) -> None:
        try:
            options = self._collect_options()
        except ValueError as exc:
            messagebox.showerror("Bake LUT", str(exc))
            return

        self.bake_button.configure(state="disabled")
        self._set_status("Baking...")
        self._log(f"Baking {options['format'].upper()} -> {options['output_path']}")
        thread = threading.Thread(target=self._bake_worker, args=(options,), daemon=True)
        thread.start()

    def _collect_options(self) -> dict:
        config_path = Path(self.config_var.get())
        output_path = Path(self.output_var.get())
        display = self.display_var.get().strip()
        view = self.view_var.get().strip()
        look_text = self.look_var.get().strip()
        input_space = self.input_space_var.get().strip()

        if not config_path:
            raise ValueError("Config path is required.")
        if self.config is None:
            raise ValueError("Load an OCIO config before baking.")
        if not output_path:
            raise ValueError("Output path is required.")
        if not display:
            raise ValueError("Display is required.")
        if not view:
            raise ValueError("View is required.")
        if not input_space:
            raise ValueError("Input space is required.")

        try:
            size = int(self.size_var.get())
        except ValueError as exc:
            raise ValueError("Size must be an integer.") from exc
        if size < 2:
            raise ValueError("Size must be at least 2.")

        return {
            "config_path": config_path,
            "output_path": output_path,
            "display": display,
            "view": view,
            "look": None if look_text in ("", NO_LOOK) else look_text,
            "input_space": input_space,
            "size": size,
            "format": self.format_var.get(),
            "flip_u": self.flip_u_var.get(),
            "flip_v": self.flip_v_var.get(),
        }

    def _bake_worker(self, options: dict) -> None:
        try:
            start = time.perf_counter()
            config = create_config(options["config_path"])
            processor = create_processor(
                config,
                input_space=options["input_space"],
                display=options["display"],
                view=options["view"],
                look=options["look"],
            )
            lut = bake_lut_rgba(processor, options["size"])
            if options["format"] == "tga":
                write_lut_tga(
                    options["output_path"],
                    lut,
                    options["size"],
                    flip_u=options["flip_u"],
                    flip_v=options["flip_v"],
                )
            else:
                write_lut_dds(options["output_path"], lut, options["size"])

            elapsed = time.perf_counter() - start
        except Exception as exc:
            self.root.after(0, lambda error=exc: self._bake_failed(error))
            return

        self.root.after(0, lambda: self._bake_finished(options["output_path"], elapsed))

    def _bake_finished(self, output_path: Path, elapsed: float) -> None:
        self.bake_button.configure(state="normal")
        self._set_status(f"Wrote {output_path}")
        self._log(f"Wrote {output_path} ({elapsed:.2f}s)")
        messagebox.showinfo("Bake LUT", f"Wrote {output_path}")

    def _bake_failed(self, exc: Exception) -> None:
        self.bake_button.configure(state="normal")
        self._set_status(f"Bake failed: {exc}")
        self._log(f"Bake failed: {exc}")
        messagebox.showerror("Bake LUT", str(exc))

    def _set_status(self, text: str) -> None:
        self.status_var.set(text)

    def _log(self, text: str) -> None:
        self.log.insert("end", text + "\n")
        self.log.see("end")


def main() -> None:
    enable_hidpi()
    root = tk.Tk()
    OCIOBakeLUTsGui(root)
    root.mainloop()


if __name__ == "__main__":
    main()
