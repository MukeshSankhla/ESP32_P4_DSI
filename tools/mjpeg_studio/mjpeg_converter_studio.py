import os
import sys
import cv2
import threading
import queue
import time
import tkinter as tk
from tkinter import ttk, filedialog, messagebox
import numpy as np
from PIL import Image, ImageTk

class BatchConvertApp(tk.Tk):
    def __init__(self):
        super().__init__()
        
        self.title("Unified Batch Converter & MJPEG Player [SYS_OS: V1.0]")
        self.geometry("1100x820")
        self.configure(bg="#06070a")
        self.minsize(950, 680)
        
        # --- SCIFI COLOR PALETTE ---
        self.bg_obsidian = "#06070a"
        self.bg_card = "#0c101b"
        self.bg_control = "#182030"
        self.bg_input = "#020408"
        self.color_cyan = "#00f0ff"
        self.color_pink = "#ff0055"
        self.color_gold = "#ffd700"
        self.fg_white = "#e2e8f0"
        self.fg_muted = "#526e84"
        
        # --- CONVERTER STATE VARIABLES ---
        self.source_dir = tk.StringVar(value="")
        self.output_dir = tk.StringVar(value="")
        self.same_as_source = tk.BooleanVar(value=True)
        
        self.prefix_var = tk.StringVar(value="a")
        self.suffix_var = tk.StringVar(value="")
        
        self.rotation_var = tk.StringVar(value="90° Clockwise")
        self.resize_var = tk.BooleanVar(value=True)
        self.width_var = tk.StringVar(value="480")
        self.height_var = tk.StringVar(value="1920")
        self.fps_var = tk.StringVar(value="24.0")
        self.quality_var = tk.IntVar(value=80)
        
        self.is_converting = False
        self.conversion_thread = None
        self.progress_queue = queue.Queue()
        self.detected_files = []
        self.tree_item_map = {}
        
        # --- PLAYER STATE VARIABLES ---
        self.player_file_data = None
        self.player_frames = []
        self.player_current_frame_idx = 0
        self.player_playing = False
        self.player_fps = 30
        self.player_loop = True
        self.player_scale = 0.35
        self.player_after_id = None
        self.player_filepath = None
        self.player_slider_pressed = False
        self.player_photo_img = None
        
        # Setup Theme Styles
        self.setup_styles()
        
        # 1. Top Telemetry Bar
        self.build_top_telemetry()
        
        # 2. Main Tab Container (Notebook)
        self.notebook = ttk.Notebook(self)
        self.notebook.pack(fill=tk.BOTH, expand=True, padx=8, pady=(4, 8))
        
        self.tab_converter = ttk.Frame(self.notebook)
        self.tab_player = ttk.Frame(self.notebook)
        
        self.notebook.add(self.tab_converter, text=" [01] BATCH CONVERTER ")
        self.notebook.add(self.tab_player, text=" [02] MJPEG PLAYER ")
        
        # Build UI for each tab
        self.build_converter_ui()
        self.build_player_ui()
        
        # Bind traces for Converter dynamic actions
        self.prefix_var.trace_add("write", lambda *args: self.update_preview())
        self.suffix_var.trace_add("write", lambda *args: self.update_preview())
        self.source_dir.trace_add("write", lambda *args: self.on_source_dir_changed())
        self.fps_var.trace_add("write", lambda *args: self.on_fps_input_changed())
        
        # Bind notebook tab selection event
        self.notebook.bind("<<NotebookTabChanged>>", self.on_tab_changed)
        
        # Bind global hotkeys, active only on player tab
        self.bind("<space>", self.on_space_key)
        self.bind("<Left>", self.on_left_key)
        self.bind("<Right>", self.on_right_key)
        
        # Initial updates
        self.update_preview()
        self.update_output_state()
        self.detect_player_scale()
        
        # Initial grid draw on player canvas
        self.player_canvas.bind("<Configure>", lambda e: self.on_canvas_resize())

    def setup_styles(self):
        self.style = ttk.Style()
        self.style.theme_use("clam")
        
        # Base setup
        self.style.configure(".", background=self.bg_obsidian, foreground=self.fg_white)
        self.style.configure("TFrame", background=self.bg_obsidian)
        self.style.configure("TLabel", background=self.bg_obsidian, foreground=self.fg_white, font=("Segoe UI", 10))
        
        # Notebook style
        self.style.configure("TNotebook", background=self.bg_obsidian, borderwidth=0)
        self.style.configure("TNotebook.Tab", background=self.bg_card, foreground=self.fg_muted, borderwidth=1, bordercolor="#1b2a47", padding=(18, 5), font=("Consolas", 9, "bold"))
        self.style.map("TNotebook.Tab",
                       background=[("selected", self.bg_card)],
                       foreground=[("selected", self.color_cyan)],
                       bordercolor=[("selected", self.color_cyan)])
        
        # Checkbutton
        self.style.configure("TCheckbutton", background=self.bg_card, foreground=self.fg_white, font=("Segoe UI", 9))
        self.style.map("TCheckbutton", 
                       background=[("active", self.bg_card)], 
                       foreground=[("active", self.color_cyan)])
        
        # Combobox
        self.style.configure("TCombobox", fieldbackground=self.bg_input, background=self.bg_control, foreground=self.fg_white, bordercolor="#1b2a47", font=("Consolas", 9))
        self.style.map("TCombobox", fieldbackground=[("readonly", self.bg_input)], foreground=[("readonly", self.fg_white)])
        
        # Flat Progressbars
        self.style.configure("TProgressbar", thickness=10, troughcolor=self.bg_input, background=self.color_cyan, borderwidth=0)
        
        # Spinbox
        self.style.configure("TSpinbox", fieldbackground=self.bg_input, foreground=self.fg_white, bordercolor="#1b2a47", arrowcolor=self.color_cyan)

        # Treeview (Cyber style)
        self.style.configure("Treeview", 
                             background=self.bg_input, 
                             foreground=self.fg_white, 
                             fieldbackground=self.bg_input,
                             rowheight=25,
                             borderwidth=0,
                             font=("Consolas", 9))
        self.style.configure("Treeview.Heading", 
                             background="#0d1424", 
                             foreground=self.color_cyan, 
                             relief="flat",
                             borderwidth=1,
                             bordercolor="#1b2a47",
                             font=("Consolas", 9, "bold"))
        self.style.map("Treeview", 
                       background=[("selected", "#182a47")], 
                       foreground=[("selected", self.color_cyan)])

    # Helper to build a cyber styled bracket frame
    def create_cyber_panel(self, parent, title):
        container = tk.Frame(parent, bg=self.bg_card, bd=0, highlightthickness=1, highlightbackground="#1b2a47")
        
        # Panel header
        header = tk.Frame(container, bg="#080b13")
        header.pack(fill=tk.X, ipady=2)
        
        # Left status indicator block
        ind = tk.Frame(header, bg=self.color_cyan, width=4)
        ind.pack(side=tk.LEFT, fill=tk.Y, padx=(4, 0), pady=4)
        
        lbl = tk.Label(header, text=f"// {title} //", bg="#080b13", fg=self.color_cyan, font=("Consolas", 10, "bold"))
        lbl.pack(side=tk.LEFT, padx=6)
        
        body = tk.Frame(container, bg=self.bg_card, padx=10, pady=10)
        body.pack(fill=tk.BOTH, expand=True)
        
        return container, body

    # Helper to build flat cyber inputs
    def create_cyber_entry(self, parent, textvariable, width=None):
        ent = tk.Entry(
            parent,
            textvariable=textvariable,
            bg=self.bg_input,
            fg=self.fg_white,
            insertbackground=self.color_cyan,
            bd=0,
            highlightthickness=1,
            highlightbackground="#182238",
            highlightcolor=self.color_cyan,
            font=("Consolas", 9),
            relief="flat"
        )
        if width is not None:
            ent.config(width=width)
        return ent

    # Helper to build customized buttons
    def create_cyber_button(self, parent, text, command, style_type="normal", width=None):
        if style_type == "accent":
            bg = self.bg_control
            fg = self.color_cyan
            abg = self.color_cyan
            afg = "#000000"
            hbg = self.color_cyan
        elif style_type == "cancel":
            bg = self.bg_control
            fg = self.color_pink
            abg = self.color_pink
            afg = "#ffffff"
            hbg = self.color_pink
        else:
            bg = self.bg_control
            fg = self.fg_white
            abg = "#1c283d"
            afg = "#ffffff"
            hbg = "#00f0ff"
            
        btn = tk.Button(
            parent,
            text=text,
            command=command,
            bg=bg,
            fg=fg,
            activebackground=abg,
            activeforeground=afg,
            bd=0,
            highlightthickness=1,
            highlightbackground="#1b2a47",
            font=("Consolas", 9, "bold"),
            relief="flat",
            cursor="hand2"
        )
        if width is not None:
            btn.config(width=width)
            
        # Bind hover transitions
        def on_enter(e):
            if btn['state'] != 'disabled':
                btn.config(highlightbackground=hbg, bg="#1c283d" if style_type == "normal" else bg)
                
        def on_leave(e):
            if btn['state'] != 'disabled':
                btn.config(highlightbackground="#1b2a47", bg=bg)
                
        btn.bind("<Enter>", on_enter)
        btn.bind("<Leave>", on_leave)
        
        return btn

    def build_top_telemetry(self):
        self.top_telemetry = tk.Frame(self, bg="#020305", height=32, bd=0, highlightthickness=1, highlightbackground="#1b2a47")
        self.top_telemetry.pack(fill=tk.X, side=tk.TOP, padx=8, pady=(8, 0))
        
        # Left Logo/Tag
        logo_lbl = tk.Label(self.top_telemetry, text="[// SYS_MATRIX_CONVERTER //]", bg="#020305", fg=self.color_cyan, font=("Consolas", 10, "bold"))
        logo_lbl.pack(side=tk.LEFT, padx=10, pady=4)
        
        # Divider line
        tk.Frame(self.top_telemetry, bg="#1b2a47", width=1).pack(side=tk.LEFT, fill=tk.Y, padx=10, pady=4)
        
        # Status field
        tk.Label(self.top_telemetry, text="SYS_STATUS:", bg="#020305", fg=self.fg_muted, font=("Consolas", 8, "bold")).pack(side=tk.LEFT, pady=4)
        self.tel_status = tk.Label(self.top_telemetry, text="ONLINE", bg="#020305", fg="#2ecc71", font=("Consolas", 8, "bold"))
        self.tel_status.pack(side=tk.LEFT, padx=(2, 10), pady=4)
        
        # Engine state field
        tk.Label(self.top_telemetry, text="ENGINE:", bg="#020305", fg=self.fg_muted, font=("Consolas", 8, "bold")).pack(side=tk.LEFT, pady=4)
        self.tel_engine = tk.Label(self.top_telemetry, text="READY", bg="#020305", fg=self.color_cyan, font=("Consolas", 8, "bold"))
        self.tel_engine.pack(side=tk.LEFT, padx=(2, 10), pady=4)
        
        # Frame Clock rate
        tk.Label(self.top_telemetry, text="SYS_CLOCK:", bg="#020305", fg=self.fg_muted, font=("Consolas", 8, "bold")).pack(side=tk.LEFT, pady=4)
        self.tel_clock = tk.Label(self.top_telemetry, text="24.0 FPS", bg="#020305", fg=self.color_gold, font=("Consolas", 8, "bold"))
        self.tel_clock.pack(side=tk.LEFT, padx=(2, 10), pady=4)
        
        # Divider line
        tk.Frame(self.top_telemetry, bg="#1b2a47", width=1).pack(side=tk.RIGHT, fill=tk.Y, padx=10, pady=4)
        
        # Active Tab field (Right alignment)
        self.tel_tab = tk.Label(self.top_telemetry, text="ACTIVE_MODULE: BATCH_CONVERTER", bg="#020305", fg=self.color_cyan, font=("Consolas", 8, "bold"))
        self.tel_tab.pack(side=tk.RIGHT, padx=10, pady=4)

    def detect_player_scale(self):
        self.update_idletasks()
        screen_height = self.winfo_screenheight()
        target_height = int(screen_height * 0.60)
        self.player_scale = round(target_height / 1920.0, 2)
        if self.player_scale <= 0.1:
            self.player_scale = 0.35
        self.player_scale_var.set(f"{int(self.player_scale * 100)}%")

    def on_tab_changed(self, event):
        selected_tab = self.notebook.tab(self.notebook.select(), "text").strip()
        self.tel_tab.config(text=f"ACTIVE_MODULE: {selected_tab.replace('[', '').replace(']', '').replace(' ', '_').upper()}")
        
        if "PLAYER" not in selected_tab:
            self.player_pause()
        else:
            self.player_scan_dir()

    def on_space_key(self, event):
        selected_tab = self.notebook.tab(self.notebook.select(), "text").strip()
        if "PLAYER" in selected_tab:
            self.player_toggle_play()

    def on_left_key(self, event):
        selected_tab = self.notebook.tab(self.notebook.select(), "text").strip()
        if "PLAYER" in selected_tab:
            self.player_step_frame(-1)

    def on_right_key(self, event):
        selected_tab = self.notebook.tab(self.notebook.select(), "text").strip()
        if "PLAYER" in selected_tab:
            self.player_step_frame(1)
            
    def on_fps_input_changed(self):
        try:
            val = float(self.fps_var.get())
            self.tel_clock.config(text=f"{val:.1f} FPS")
        except:
            pass

    # =========================================================================
    # TAB 1: BATCH CONVERTER UI & LOGIC
    # =========================================================================
    def build_converter_ui(self):
        self.tab_converter.columnconfigure(0, weight=3)
        self.tab_converter.columnconfigure(1, weight=5)
        self.tab_converter.rowconfigure(0, weight=1)
        
        # --- Left Column (Settings Panel) ---
        self.conv_left = tk.Frame(self.tab_converter, bg=self.bg_obsidian)
        self.conv_left.grid(row=0, column=0, sticky="nsew", padx=(5, 5), pady=8)
        
        # 0. Quick Presets Panel
        preset_p, preset_body = self.create_cyber_panel(self.conv_left, "00_SD_CARD_QUICK_PRESETS")
        preset_p.pack(fill=tk.X, pady=(0, 8))
        preset_body.columnconfigure(0, weight=1)
        preset_body.columnconfigure(1, weight=1)
        
        btn_preset_vert = self.create_cyber_button(preset_body, "[ VERTICAL SD (v1) ]", self.apply_preset_vertical)
        btn_preset_vert.grid(row=0, column=0, sticky="ew", padx=(0, 2), pady=2)
        
        btn_preset_horiz = self.create_cyber_button(preset_body, "[ HORIZONTAL SD (h1) ]", self.apply_preset_horizontal)
        btn_preset_horiz.grid(row=0, column=1, sticky="ew", padx=(2, 0), pady=2)
        
        # 1. Folder Configurations Panel
        dir_p, dir_body = self.create_cyber_panel(self.conv_left, "01_FOLDER_CONFIGURATION")
        dir_p.pack(fill=tk.X, pady=(0, 10))
        dir_body.columnconfigure(1, weight=1)
        
        ttk.Label(dir_body, text="Source Dir:", font=("Consolas", 9), background=self.bg_card).grid(row=0, column=0, sticky="w", pady=2)
        self.ent_source = self.create_cyber_entry(dir_body, self.source_dir)
        self.ent_source.grid(row=0, column=1, sticky="ew", padx=5, pady=2)
        self.create_cyber_button(dir_body, " BROWSE ", self.browse_source).grid(row=0, column=2, pady=2, padx=(2, 0))
        
        self.chk_same = ttk.Checkbutton(dir_body, text="Redirect transcode files to source folder", 
                                        variable=self.same_as_source, command=self.update_output_state)
        self.chk_same.grid(row=1, column=0, columnspan=3, sticky="w", pady=(6, 6))
        
        self.lbl_output = ttk.Label(dir_body, text="Output Dir:", font=("Consolas", 9), background=self.bg_card)
        self.lbl_output.grid(row=2, column=0, sticky="w", pady=2)
        self.ent_output = self.create_cyber_entry(dir_body, self.output_dir)
        self.ent_output.grid(row=2, column=1, sticky="ew", padx=5, pady=2)
        self.btn_output_browse = self.create_cyber_button(dir_body, " BROWSE ", self.browse_output)
        self.btn_output_browse.grid(row=2, column=2, pady=2, padx=(2, 0))
        
        # 2. Output Naming Options Panel
        naming_p, naming_body = self.create_cyber_panel(self.conv_left, "02_NAMING_PARAMETERS")
        naming_p.pack(fill=tk.X, pady=(0, 10))
        naming_body.columnconfigure(1, weight=1)
        
        ttk.Label(naming_body, text="Prefix tag:", font=("Consolas", 9), background=self.bg_card).grid(row=0, column=0, sticky="w", pady=2)
        self.ent_prefix = self.create_cyber_entry(naming_body, self.prefix_var, width=15)
        self.ent_prefix.grid(row=0, column=1, sticky="w", padx=5, pady=2)
        
        ttk.Label(naming_body, text="Suffix tag:", font=("Consolas", 9), background=self.bg_card).grid(row=1, column=0, sticky="w", pady=2)
        self.ent_suffix = self.create_cyber_entry(naming_body, self.suffix_var, width=15)
        self.ent_suffix.grid(row=1, column=1, sticky="w", padx=5, pady=2)
        
        self.lbl_preview = ttk.Label(naming_body, text="Preview: video.mp4 -> avideo.mjpeg", style="Subtitle.TLabel", background=self.bg_card)
        self.lbl_preview.grid(row=2, column=0, columnspan=2, sticky="w", pady=(8, 2))
        
        # 3. Video Transcode Options Panel
        video_p, video_body = self.create_cyber_panel(self.conv_left, "03_TRANSCODE_MATRIX")
        video_p.pack(fill=tk.X, pady=(0, 10))
        video_body.columnconfigure(1, weight=1)
        
        ttk.Label(video_body, text="Rotation:", font=("Consolas", 9), background=self.bg_card).grid(row=0, column=0, sticky="w", pady=2)
        self.cmb_rotation = ttk.Combobox(video_body, textvariable=self.rotation_var, state="readonly",
                                         values=["No Rotation", "90° Clockwise", "180°", "90° Counter-Clockwise"])
        self.cmb_rotation.grid(row=0, column=1, columnspan=2, sticky="ew", padx=5, pady=2)
        
        self.chk_resize = ttk.Checkbutton(video_body, text="Apply matrix resolution overrides", 
                                          variable=self.resize_var, command=self.update_resize_state)
        self.chk_resize.grid(row=1, column=0, columnspan=3, sticky="w", pady=(6, 6))
        
        self.lbl_width = ttk.Label(video_body, text="Res Width:", font=("Consolas", 9), background=self.bg_card)
        self.lbl_width.grid(row=2, column=0, sticky="w", pady=2)
        self.ent_width = self.create_cyber_entry(video_body, self.width_var, width=10)
        self.ent_width.grid(row=2, column=1, sticky="w", padx=5, pady=2)
        
        self.lbl_height = ttk.Label(video_body, text="Res Height:", font=("Consolas", 9), background=self.bg_card)
        self.lbl_height.grid(row=3, column=0, sticky="w", pady=2)
        self.ent_height = self.create_cyber_entry(video_body, self.height_var, width=10)
        self.ent_height.grid(row=3, column=1, sticky="w", padx=5, pady=2)
        
        ttk.Label(video_body, text="Target FPS:", font=("Consolas", 9), background=self.bg_card).grid(row=4, column=0, sticky="w", pady=2)
        self.ent_fps = self.create_cyber_entry(video_body, self.fps_var, width=10)
        self.ent_fps.grid(row=4, column=1, sticky="w", padx=5, pady=2)
        
        ttk.Label(video_body, text="JPG Quality:", font=("Consolas", 9), background=self.bg_card).grid(row=5, column=0, sticky="w", pady=2)
        self.scale_quality = ttk.Scale(video_body, from_=10, to=100, variable=self.quality_var, orient=tk.HORIZONTAL)
        self.scale_quality.grid(row=5, column=1, sticky="ew", padx=5, pady=2)
        self.lbl_quality_val = ttk.Label(video_body, text="80%", font=("Consolas", 9, "bold"), background=self.bg_card, foreground=self.color_cyan)
        self.lbl_quality_val.grid(row=5, column=2, sticky="w", pady=2, padx=4)
        
        self.quality_var.trace_add("write", lambda *args: self.lbl_quality_val.config(text=f"{self.quality_var.get()}%"))
        
        # Bottom Buttons
        btn_frame = tk.Frame(self.conv_left, bg=self.bg_obsidian)
        btn_frame.pack(fill=tk.X, side=tk.BOTTOM, pady=2)
        
        self.btn_convert = self.create_cyber_button(btn_frame, "/// [ START BATCH CONVERSION ] ///", self.start_conversion, style_type="accent")
        self.btn_convert.pack(fill=tk.X, pady=4, ipady=6)
        
        self.btn_cancel = self.create_cyber_button(btn_frame, "/// [ TERMINATE PROCESS ] ///", self.cancel_conversion, style_type="cancel")
        self.btn_cancel.pack(fill=tk.X, pady=2, ipady=6)
        
        # --- Right Column (Files & Progress) ---
        self.conv_right = tk.Frame(self.tab_converter, bg=self.bg_obsidian)
        self.conv_right.grid(row=0, column=1, sticky="nsew", padx=(5, 5), pady=8)
        self.conv_right.columnconfigure(0, weight=1)
        self.conv_right.rowconfigure(0, weight=2)
        self.conv_right.rowconfigure(1, weight=1)
        
        # Detected Files Panel
        files_p, files_body = self.create_cyber_panel(self.conv_right, "04_BATCH_QUEUE_DETECTOR")
        files_p.grid(row=0, column=0, sticky="nsew", pady=(0, 10))
        files_body.columnconfigure(0, weight=1)
        files_body.rowconfigure(0, weight=1)
        
        tree_container = tk.Frame(files_body, bg=self.bg_input)
        tree_container.grid(row=0, column=0, sticky="nsew")
        tree_container.columnconfigure(0, weight=1)
        tree_container.rowconfigure(0, weight=1)
        
        self.tree = ttk.Treeview(tree_container, columns=("name", "size", "status"), show="headings")
        self.tree.grid(row=0, column=0, sticky="nsew")
        self.tree.heading("name", text="File Index Name")
        self.tree.heading("size", text="Raw Weight")
        self.tree.heading("status", text="Engine State")
        self.tree.column("name", width=250, anchor="w")
        self.tree.column("size", width=100, anchor="center")
        self.tree.column("status", width=120, anchor="center")
        
        tree_scroll = ttk.Scrollbar(tree_container, orient=tk.VERTICAL, command=self.tree.yview)
        tree_scroll.grid(row=0, column=1, sticky="ns")
        self.tree.config(yscrollcommand=tree_scroll.set)
        
        self.lbl_file_stats = ttk.Label(files_body, text="Diagnostic: No matrices detected.", style="Subtitle.TLabel", background=self.bg_card)
        self.lbl_file_stats.grid(row=1, column=0, sticky="w", pady=(5, 0))
        
        # Log Output & Progress Panel
        status_p, status_body = self.create_cyber_panel(self.conv_right, "05_LOGS_AND_DIAGNOSTICS")
        status_p.grid(row=1, column=0, sticky="nsew")
        status_body.columnconfigure(0, weight=1)
        status_body.rowconfigure(2, weight=1)
        
        prog_grid = tk.Frame(status_body, bg=self.bg_card)
        prog_grid.grid(row=0, column=0, sticky="ew", pady=(0, 5))
        prog_grid.columnconfigure(1, weight=1)
        
        self.lbl_overall = ttk.Label(prog_grid, text="OVERALL_JOB_STAT:", font=("Consolas", 8, "bold"), background=self.bg_card, foreground=self.fg_muted)
        self.lbl_overall.grid(row=0, column=0, sticky="w", padx=2, pady=1)
        self.bar_overall = ttk.Progressbar(prog_grid, mode="determinate")
        self.bar_overall.grid(row=0, column=1, sticky="ew", padx=5, pady=1)
        
        self.lbl_current = ttk.Label(prog_grid, text="CURRENT_FILE_STAT:", font=("Consolas", 8, "bold"), background=self.bg_card, foreground=self.fg_muted)
        self.lbl_current.grid(row=1, column=0, sticky="w", padx=2, pady=1)
        self.bar_current = ttk.Progressbar(prog_grid, mode="determinate")
        self.bar_current.grid(row=1, column=1, sticky="ew", padx=5, pady=1)
        
        log_container = tk.Frame(status_body, bg=self.bg_input, bd=0, highlightthickness=1, highlightbackground="#1b2a47")
        log_container.grid(row=2, column=0, sticky="nsew", pady=(5, 0))
        log_container.columnconfigure(0, weight=1)
        log_container.rowconfigure(0, weight=1)
        
        self.log_text = tk.Text(log_container, bg=self.bg_input, fg=self.color_cyan, bd=0, 
                                font=("Consolas", 9), wrap=tk.WORD, highlightthickness=0,
                                insertbackground=self.color_cyan)
        self.log_text.grid(row=0, column=0, sticky="nsew")
        
        self.log_text.tag_config("info", foreground=self.fg_white)
        self.log_text.tag_config("success", foreground=self.color_cyan)
        self.log_text.tag_config("error", foreground=self.color_pink)
        self.log_text.tag_config("warning", foreground=self.color_gold)
        
        log_scroll = ttk.Scrollbar(log_container, orient=tk.VERTICAL, command=self.log_text.yview)
        log_scroll.grid(row=0, column=1, sticky="ns")
        self.log_text.config(yscrollcommand=log_scroll.set)

    def apply_preset_vertical(self):
        self.prefix_var.set("v")
        self.suffix_var.set("")
        self.rotation_var.set("No Rotation")
        self.resize_var.set(True)
        self.width_var.set("480")
        self.height_var.set("1920")
        self.log_to_box("Applied Preset: VERTICAL SD (/Vertical/v1.mjpeg - 480x1920 No Rotation)\n", "warning")

    def apply_preset_horizontal(self):
        self.prefix_var.set("h")
        self.suffix_var.set("")
        self.rotation_var.set("90° Clockwise")
        self.resize_var.set(True)
        self.width_var.set("480")
        self.height_var.set("1920")
        self.log_to_box("Applied Preset: HORIZONTAL SD (/Horizontal/h1.mjpeg - 480x1920 90° CW)\n", "warning")

    def update_output_state(self):
        if self.same_as_source.get():
            self.output_dir.set(self.source_dir.get())
            self.ent_output.config(state="disabled")
            self.btn_output_browse.config(state="disabled", highlightbackground="#101826")
            self.lbl_output.config(foreground=self.fg_muted)
        else:
            self.ent_output.config(state="normal")
            self.btn_output_browse.config(state="normal")
            self.lbl_output.config(foreground=self.fg_white)

    def update_resize_state(self):
        if self.resize_var.get():
            self.ent_width.config(state="normal")
            self.ent_height.config(state="normal")
            self.lbl_width.config(foreground=self.fg_white)
            self.lbl_height.config(foreground=self.fg_white)
        else:
            self.ent_width.config(state="disabled")
            self.ent_height.config(state="disabled")
            self.lbl_width.config(foreground=self.fg_muted)
            self.lbl_height.config(foreground=self.fg_muted)

    def browse_source(self):
        initial = self.source_dir.get() if self.source_dir.get() else os.getcwd()
        path = filedialog.askdirectory(title="Select Source Videos Directory", initialdir=initial)
        if path:
            path = os.path.normpath(path)
            self.source_dir.set(path)
            if self.same_as_source.get():
                self.output_dir.set(path)

    def browse_output(self):
        initial = self.output_dir.get() if self.output_dir.get() else os.getcwd()
        path = filedialog.askdirectory(title="Select Output Directory", initialdir=initial)
        if path:
            path = os.path.normpath(path)
            self.output_dir.set(path)

    def update_preview(self):
        prefix = self.prefix_var.get()
        suffix = self.suffix_var.get()
        self.lbl_preview.config(text=f"Preview: source_video.mp4 ➔ {prefix}source_video{suffix}.mjpeg")

    def on_source_dir_changed(self):
        path = self.source_dir.get()
        if not path or not os.path.isdir(path):
            self.clear_file_list()
            return
            
        self.clear_file_list()
        
        valid_exts = (".mp4", ".avi", ".mov", ".mkv", ".m4v", ".webm", ".flv")
        try:
            raw_files = os.listdir(path)
        except Exception as e:
            self.log_to_box(f"Error accessing matrix directory: {str(e)}\n", "error")
            return
            
        videos = [f for f in raw_files if f.lower().endswith(valid_exts)]
        
        def get_num(filename):
            name = os.path.splitext(filename)[0]
            try:
                return (0, int(name))
            except ValueError:
                return (1, filename)

        videos.sort(key=get_num)
        self.detected_files = videos
        
        if not videos:
            self.lbl_file_stats.config(text="Diagnostic: No transcodeable videos detected.")
            return
            
        for f in videos:
            full_path = os.path.join(path, f)
            try:
                sz = os.path.getsize(full_path)
                size_str = f"{sz / (1024 * 1024):.1f} MB"
            except:
                size_str = "Unknown"
            
            item_id = self.tree.insert("", tk.END, values=(f, size_str, "AWAITING"))
            self.tree_item_map[f] = item_id
            
        self.lbl_file_stats.config(text=f"Diagnostic: Identified {len(videos)} matrices ready for transcode.")
        self.log_to_box(f"Identified {len(videos)} transcode task files in queue.\n", "info")

    def clear_file_list(self):
        self.tree.delete(*self.tree.get_children())
        self.tree_item_map.clear()
        self.detected_files.clear()
        self.lbl_file_stats.config(text="Diagnostic: No matrices detected.")

    def log_to_box(self, text, level="info"):
        self.log_text.config(state="normal")
        timestamp = time.strftime("[%H:%M:%S] ")
        self.log_text.insert(tk.END, timestamp, "info")
        self.log_text.insert(tk.END, text, level)
        self.log_text.see(tk.END)
        self.log_text.config(state="disabled")

    def toggle_ui_state(self, normal=True):
        state = "normal" if normal else "disabled"
        self.ent_source.config(state=state)
        self.chk_same.config(state=state)
        if not self.same_as_source.get():
            self.ent_output.config(state=state)
            self.btn_output_browse.config(state=state)
        self.ent_prefix.config(state=state)
        self.ent_suffix.config(state=state)
        self.cmb_rotation.config(state=state)
        self.chk_resize.config(state=state)
        if self.resize_var.get():
            self.ent_width.config(state=state)
            self.ent_height.config(state=state)
        self.ent_fps.config(state=state)
        self.scale_quality.config(state=state)
        
        if normal:
            self.btn_convert.config(state="normal")
            self.btn_cancel.config(state="disabled")
            self.tel_engine.config(text="READY", fg=self.color_cyan)
        else:
            self.btn_convert.config(state="disabled")
            self.btn_cancel.config(state="normal")
            self.tel_engine.config(text="ACTIVE", fg=self.color_pink)

    def validate_inputs(self):
        src = self.source_dir.get()
        if not src or not os.path.isdir(src):
            messagebox.showerror("System Config Error", "Target source matrix folder is missing or invalid.")
            return False
            
        out = self.output_dir.get()
        if not out:
            messagebox.showerror("System Config Error", "Transcode output target directory not configured.")
            return False
            
        if not os.path.exists(out):
            try:
                os.makedirs(out, exist_ok=True)
            except Exception as e:
                messagebox.showerror("System Config Error", f"Failed to mount output directory path:\n{str(e)}")
                return False
                
        if not self.detected_files:
            messagebox.showerror("System Config Error", "Empty task pipeline. No video matrices detected.")
            return False
            
        try:
            fps = float(self.fps_var.get())
            if fps <= 0:
                raise ValueError()
        except ValueError:
            messagebox.showerror("System Config Error", "Engine speed multiplier (FPS) must be a positive numeric value.")
            return False
            
        if self.resize_var.get():
            try:
                w = int(self.width_var.get())
                h = int(self.height_var.get())
                if w <= 0 or h <= 0:
                    raise ValueError()
            except ValueError:
                messagebox.showerror("System Config Error", "Resolution override vectors (Width/Height) must be integer types.")
                return False
                
        return True

    def start_conversion(self):
        if not self.validate_inputs():
            return
            
        rotation_map = {
            "No Rotation": None,
            "90° Clockwise": cv2.ROTATE_90_CLOCKWISE,
            "180°": cv2.ROTATE_180,
            "90° Counter-Clockwise": cv2.ROTATE_90_COUNTERCLOCKWISE
        }
        
        config = {
            "source_dir": self.source_dir.get(),
            "output_dir": self.output_dir.get(),
            "prefix": self.prefix_var.get(),
            "suffix": self.suffix_var.get(),
            "rotation": rotation_map[self.rotation_var.get()],
            "resize": self.resize_var.get(),
            "width": int(self.width_var.get()) if self.resize_var.get() else 480,
            "height": int(self.height_var.get()) if self.resize_var.get() else 1920,
            "fps": float(self.fps_var.get()),
            "quality": self.quality_var.get()
        }
        
        for f, item_id in self.tree_item_map.items():
            size_val = self.tree.item(item_id, 'values')[1]
            self.tree.item(item_id, values=(f, size_val, "PENDING"))
            
        self.bar_overall.config(value=0)
        self.bar_current.config(value=0)
        
        self.is_converting = True
        self.toggle_ui_state(normal=False)
        self.log_to_box("/// INITIALIZING BATCH TRANSCODE MATRIX PROCESS ///\n", "warning")
        
        file_tasks = [(os.path.join(config['source_dir'], f), f) for f in self.detected_files]
        
        self.conversion_thread = threading.Thread(
            target=self.conversion_worker, 
            args=(file_tasks, config), 
            daemon=True
        )
        self.conversion_thread.start()
        
        self.after(100, self.update_gui_from_thread)

    def cancel_conversion(self):
        if not self.is_converting:
            return
        self.is_converting = False
        self.log_to_box("/// SIGNAL SENT: ABORT CORE THREAD. CLEANING REGISTERS... ///\n", "warning")
        self.btn_cancel.config(state="disabled")

    def conversion_worker(self, file_tasks, config):
        total_files = len(file_tasks)
        success_count = 0
        
        for idx, task in enumerate(file_tasks):
            if not self.is_converting:
                break
                
            input_path, relative_name = task
            base_name, _ = os.path.splitext(relative_name)
            output_name = f"{config['prefix']}{base_name}{config['suffix']}.mjpeg"
            output_path = os.path.join(config['output_dir'], output_name)
            
            self.progress_queue.put(('start_file', relative_name, idx))
            
            success = self.convert_single_video(input_path, output_path, config)
            
            if not self.is_converting:
                self.progress_queue.put(('file_cancelled', relative_name, idx))
                break
                
            if success:
                success_count += 1
                self.progress_queue.put(('file_done', relative_name, idx))
            else:
                self.progress_queue.put(('file_error', relative_name, idx))
                
        self.progress_queue.put(('batch_done', success_count, total_files))

    def convert_single_video(self, input_path, output_path, config):
        cap = cv2.VideoCapture(input_path)
        if not cap.isOpened():
            self.progress_queue.put(('log', f"FAIL: Thread unable to mount {os.path.basename(input_path)}\n", "error"))
            return False
            
        total_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
        fps = cap.get(cv2.CAP_PROP_FPS)
        
        target_fps = config['fps']
        duration = total_frames / fps if fps > 0 else 0
        total_target_frames = int(round(duration * target_fps))
        
        if total_target_frames <= 0:
            total_target_frames = 1
            
        self.progress_queue.put(('log', f"PROCESS: Transcoding {os.path.basename(input_path)} -> {os.path.basename(output_path)}\n", "info"))
        self.progress_queue.put(('log', f"  Matrix frames size: {total_target_frames} at Clock: {target_fps} FPS\n", "info"))
        
        try:
            with open(output_path, "wb") as f:
                target_idx = 0
                input_idx = 0
                ret, frame = cap.read()
                
                while ret and target_idx < total_target_frames:
                    if not self.is_converting:
                        cap.release()
                        try:
                            if os.path.exists(output_path):
                                os.remove(output_path)
                        except:
                            pass
                        return False
                        
                    needed_input_idx = int(target_idx * fps / target_fps)
                    
                    while input_idx < needed_input_idx:
                        ret, frame = cap.read()
                        input_idx += 1
                        if not ret:
                            break
                            
                    if not ret or frame is None:
                        break
                        
                    if config['rotation'] is not None:
                        rotated = cv2.rotate(frame, config['rotation'])
                    else:
                        rotated = frame
                        
                    if config['resize']:
                        resized = cv2.resize(rotated, (config['width'], config['height']))
                    else:
                        resized = rotated
                        
                    ret_val, jpeg_bytes = cv2.imencode(".jpg", resized, [cv2.IMWRITE_JPEG_QUALITY, config['quality']])
                    if ret_val:
                        f.write(jpeg_bytes.tobytes())
                        
                    target_idx += 1
                    
                    if target_idx % 15 == 0 or target_idx == total_target_frames:
                        self.progress_queue.put(('file_progress', target_idx, total_target_frames))
                        
            cap.release()
            
            if os.path.exists(output_path) and os.path.getsize(output_path) > 0:
                size_mb = os.path.getsize(output_path) / (1024 * 1024)
                self.progress_queue.put(('log', f"  SUCCESS: Stream flushed to {os.path.basename(output_path)} ({size_mb:.2f} MB)\n", "success"))
                return True
            else:
                self.progress_queue.put(('log', f"  FAIL: Empty byte block generated for {os.path.basename(input_path)}\n", "error"))
                return False
                
        except Exception as e:
            cap.release()
            self.progress_queue.put(('log', f"  EXCEPTION encountered during process: {str(e)}\n", "error"))
            try:
                if os.path.exists(output_path):
                    os.remove(output_path)
            except:
                pass
            return False

    def update_gui_from_thread(self):
        try:
            while True:
                msg = self.progress_queue.get_nowait()
                msg_type = msg[0]
                
                if msg_type == 'log':
                    _, log_txt, level = msg
                    self.log_to_box(log_txt, level)
                    
                elif msg_type == 'start_file':
                    _, filename, index = msg
                    item_id = self.tree_item_map.get(filename)
                    if item_id:
                        size_val = self.tree.item(item_id, 'values')[1]
                        self.tree.item(item_id, values=(filename, size_val, "CONVERTING"))
                        self.tree.see(item_id)
                        self.tree.selection_set(item_id)
                    self.lbl_overall.config(text=f"OVERALL_JOB_STAT: file {index + 1}/{len(self.detected_files)}")
                    self.bar_current.config(value=0)
                    
                elif msg_type == 'file_progress':
                    _, current_frame, total_frames = msg
                    percentage = (current_frame / total_frames) * 100
                    self.bar_current.config(value=percentage)
                    self.lbl_current.config(text=f"CURRENT_FILE_STAT: frame {current_frame}/{total_frames} ({percentage:.1f}%)")
                    
                elif msg_type == 'file_done':
                    _, filename, index = msg
                    item_id = self.tree_item_map.get(filename)
                    if item_id:
                        size_val = self.tree.item(item_id, 'values')[1]
                        self.tree.item(item_id, values=(filename, size_val, "FINISHED"))
                    
                    overall_percentage = ((index + 1) / len(self.detected_files)) * 100
                    self.bar_overall.config(value=overall_percentage)
                    
                elif msg_type == 'file_cancelled':
                    _, filename, index = msg
                    item_id = self.tree_item_map.get(filename)
                    if item_id:
                        size_val = self.tree.item(item_id, 'values')[1]
                        self.tree.item(item_id, values=(filename, size_val, "ABORTED"))
                        
                elif msg_type == 'file_error':
                    _, filename, index = msg
                    item_id = self.tree_item_map.get(filename)
                    if item_id:
                        size_val = self.tree.item(item_id, 'values')[1]
                        self.tree.item(item_id, values=(filename, size_val, "CRITICAL"))
                    
                    overall_percentage = ((index + 1) / len(self.detected_files)) * 100
                    self.bar_overall.config(value=overall_percentage)
                    
                elif msg_type == 'batch_done':
                    _, success_count, total_count = msg
                    self.is_converting = False
                    self.toggle_ui_state(normal=True)
                    self.bar_current.config(value=0)
                    self.lbl_current.config(text="CURRENT_FILE_STAT:")
                    
                    if success_count == total_count:
                        self.log_to_box(f"\n/// PROCESS TERMINATED SUCCESSFULLY: TRANSCODED {success_count}/{total_count} MATRICES ///\n", "success")
                        messagebox.showinfo("Matrix Done", f"Engine transcode sequence finished. {success_count} MJPEG output streams created.")
                    elif self.is_converting == False and success_count < total_count:
                        self.log_to_box(f"\n/// BATCH INTERRUPTED: SAVED {success_count}/{total_count} MATRICES ///\n", "warning")
                        messagebox.showwarning("Matrix Interrupted", f"Transcode process aborted. Saved {success_count}/{total_count} files.")
                    else:
                        self.log_to_box(f"\n/// PROCESS COMPLETED WITH ERRORS: TRANSCODED {success_count}/{total_count} MATRICES ///\n", "error")
                        messagebox.showerror("Matrix Failure", f"Engine transcode finished with warnings. Successfully saved {success_count}/{total_count} files.")
                    
                    self.on_source_dir_changed()
                    return
                    
        except queue.Empty:
            pass
            
        if self.is_converting:
            self.after(100, self.update_gui_from_thread)

    # =========================================================================
    # TAB 2: MJPEG PLAYER UI & LOGIC
    # =========================================================================
    def build_player_ui(self):
        self.player_paned = ttk.PanedWindow(self.tab_player, orient=tk.HORIZONTAL)
        self.player_paned.pack(fill=tk.BOTH, expand=True, padx=5, pady=5)
        
        # --- Left Panel: Playlist ---
        self.player_left = tk.Frame(self.player_paned, width=240, bg=self.bg_obsidian)
        self.player_paned.add(self.player_left, weight=0)
        
        playlist_p, playlist_body = self.create_cyber_panel(self.player_left, "MJPEG_STREAM_PLAYLIST")
        playlist_p.pack(fill=tk.BOTH, expand=True)
        playlist_body.columnconfigure(0, weight=1)
        playlist_body.rowconfigure(0, weight=1)
        
        self.player_listbox_frame = tk.Frame(playlist_body, bg=self.bg_input, bd=0, highlightthickness=1, highlightbackground="#1b2a47")
        self.player_listbox_frame.grid(row=0, column=0, sticky="nsew")
        
        self.player_listbox = tk.Listbox(
            self.player_listbox_frame, 
            bg=self.bg_input, 
            fg=self.fg_white, 
            selectbackground="#182a47", 
            selectforeground=self.color_cyan, 
            bd=0, 
            highlightthickness=0,
            font=("Consolas", 9)
        )
        self.player_listbox.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        self.player_listbox.bind("<<ListboxSelect>>", self.on_player_select_file)
        
        player_list_scroll = ttk.Scrollbar(self.player_listbox_frame, orient=tk.VERTICAL, command=self.player_listbox.yview)
        player_list_scroll.pack(side=tk.RIGHT, fill=tk.Y)
        self.player_listbox.config(yscrollcommand=player_list_scroll.set)
        
        player_btn_frame = tk.Frame(playlist_body, bg=self.bg_card)
        player_btn_frame.grid(row=1, column=0, sticky="ew", pady=(8, 0))
        
        self.player_btn_browse = self.create_cyber_button(player_btn_frame, "[ OPEN MJPEG VECTOR ]", self.player_browse_file)
        self.player_btn_browse.pack(fill=tk.X, pady=2)
        
        self.player_btn_refresh = self.create_cyber_button(player_btn_frame, "[ RELOAD STREAM DIRECTORY ]", self.player_scan_dir)
        self.player_btn_refresh.pack(fill=tk.X, pady=2)
        
        # --- Right Panel: Display Screen & Control Interface ---
        self.player_right = tk.Frame(self.player_paned, bg=self.bg_obsidian)
        self.player_paned.add(self.player_right, weight=1)
        
        disp_p, disp_body = self.create_cyber_panel(self.player_right, "DISPLAY_STREAM_MATRIX")
        disp_p.pack(fill=tk.BOTH, expand=True)
        disp_body.columnconfigure(0, weight=1)
        disp_body.rowconfigure(2, weight=1) # canvas frame expands
        
        player_info_frame = tk.Frame(disp_body, bg=self.bg_card)
        player_info_frame.grid(row=0, column=0, sticky="ew", pady=(0, 5))
        
        self.player_lbl_title = ttk.Label(player_info_frame, text="NO_STREAM_MOUNTED", font=("Consolas", 10, "bold"), background=self.bg_card, foreground=self.color_pink)
        self.player_lbl_title.pack(anchor=tk.W)
        
        self.player_lbl_stats = ttk.Label(player_info_frame, text="SYS_MATRIX_AWAITING_INPUT_DIAGNOSTICS...", style="Subtitle.TLabel", background=self.bg_card)
        self.player_lbl_stats.pack(anchor=tk.W, pady=(2, 0))
        
        # Timeline Scale
        self.player_timeline_frame = tk.Frame(disp_body, bg=self.bg_card)
        self.player_timeline_frame.grid(row=1, column=0, sticky="ew", pady=(2, 5))
        
        self.player_lbl_time_cur = ttk.Label(self.player_timeline_frame, text="0000", font=("Consolas", 9, "bold"), background=self.bg_card, foreground=self.color_cyan)
        self.player_lbl_time_cur.pack(side=tk.LEFT, padx=5)
        
        self.player_slider_var = tk.DoubleVar()
        self.player_slider = ttk.Scale(self.player_timeline_frame, from_=0, to=100, variable=self.player_slider_var, command=self.on_player_slider_move)
        self.player_slider.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=5)
        self.player_slider.bind("<ButtonRelease-1>", self.on_player_slider_release)
        self.player_slider.bind("<Button-1>", self.on_player_slider_press)
        
        self.player_lbl_time_total = ttk.Label(self.player_timeline_frame, text="0000", font=("Consolas", 9, "bold"), background=self.bg_card, foreground=self.fg_muted)
        self.player_lbl_time_total.pack(side=tk.RIGHT, padx=5)
        
        # Display Screen Box
        self.player_canvas_container = tk.Frame(disp_body, bg=self.bg_input, bd=0, highlightthickness=1, highlightbackground="#1b2a47")
        self.player_canvas_container.grid(row=2, column=0, sticky="nsew", pady=5)
        
        self.player_canvas = tk.Canvas(self.player_canvas_container, bg="#020305", bd=0, highlightthickness=0)
        self.player_canvas.pack(fill=tk.BOTH, expand=True)
        
        # Bottom controls row
        self.player_ctrls_row = tk.Frame(disp_body, bg=self.bg_card)
        self.player_ctrls_row.grid(row=3, column=0, sticky="ew", pady=(8, 0))
        
        self.player_btn_prev = self.create_cyber_button(self.player_ctrls_row, "⏮", lambda: self.player_step_frame(-1), width=4)
        self.player_btn_prev.pack(side=tk.LEFT, padx=2)
        
        self.player_btn_play = self.create_cyber_button(self.player_ctrls_row, "▶ PLAY", self.player_toggle_play, style_type="accent", width=10)
        self.player_btn_play.pack(side=tk.LEFT, padx=2)
        
        self.player_btn_next = self.create_cyber_button(self.player_ctrls_row, "⏭", lambda: self.player_step_frame(1), width=4)
        self.player_btn_next.pack(side=tk.LEFT, padx=2)
        
        self.player_btn_stop = self.create_cyber_button(self.player_ctrls_row, "⏹ STOP", self.player_stop, width=8)
        self.player_btn_stop.pack(side=tk.LEFT, padx=5)
        
        # Vector separator line
        tk.Frame(self.player_ctrls_row, bg="#1b2a47", width=1).pack(side=tk.LEFT, fill=tk.Y, padx=8, pady=2)
        
        # Spinbox & Combobox details
        ttk.Label(self.player_ctrls_row, text="FPS:", font=("Consolas", 9, "bold"), background=self.bg_card, foreground=self.color_cyan).pack(side=tk.LEFT, padx=2)
        self.player_fps_var = tk.IntVar(value=30)
        self.player_fps_spin = ttk.Spinbox(self.player_ctrls_row, from_=1, to=120, width=5, textvariable=self.player_fps_var, command=self.on_player_fps_changed)
        self.player_fps_spin.pack(side=tk.LEFT, padx=2)
        self.player_fps_spin.bind("<Return>", lambda e: self.on_player_fps_changed())
        
        ttk.Label(self.player_ctrls_row, text="SCALE:", font=("Consolas", 9, "bold"), background=self.bg_card, foreground=self.color_cyan).pack(side=tk.LEFT, padx=8)
        self.player_scale_var = tk.StringVar(value="35%")
        self.player_scale_combo = ttk.Combobox(self.player_ctrls_row, values=["15%", "25%", "35%", "50%", "75%", "100%"], textvariable=self.player_scale_var, width=6, state="readonly")
        self.player_scale_combo.pack(side=tk.LEFT, padx=2)
        self.player_scale_combo.bind("<<ComboboxSelected>>", self.on_player_scale_changed)
        
        self.player_chk_loop = ttk.Checkbutton(self.player_ctrls_row, text="LOOP_LOCK", variable=tk.BooleanVar(value=True), command=self.player_toggle_loop)
        self.player_chk_loop.pack(side=tk.RIGHT, padx=5)

    def player_scan_dir(self):
        self.player_listbox.delete(0, tk.END)
        self.player_file_map = {}
        
        repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
        custom_dir = self.source_dir.get()
        
        candidate_dirs = []
        if custom_dir and os.path.exists(custom_dir):
            candidate_dirs.append(custom_dir)
        candidate_dirs.extend([
            "Animations",
            os.path.join(repo_root, "Animations"),
            os.path.join(repo_root, "Animations", "Vertical"),
            os.path.join(repo_root, "Animations", "Horizontal"),
            "."
        ])
        
        found = False
        for search_dir in candidate_dirs:
            if os.path.exists(search_dir) and os.path.isdir(search_dir):
                try:
                    files = [f for f in os.listdir(search_dir) if f.lower().endswith(".mjpeg")]
                    if files:
                        found = True
                        for f in sorted(files):
                            disp_name = f if search_dir in ("Animations", os.path.join(repo_root, "Animations"), ".") else f"{os.path.basename(search_dir)}/{f}"
                            if disp_name not in self.player_file_map:
                                self.player_file_map[disp_name] = os.path.join(search_dir, f)
                                self.player_listbox.insert(tk.END, disp_name)
                except Exception:
                    pass
        if not found:
            self.player_lbl_stats.config(text="STATUS: No .mjpeg files found. Use Browse to load files.")

    def player_browse_file(self):
        filepath = filedialog.askopenfilename(
            title="Open MJPEG Video File",
            filetypes=[("MJPEG Video", "*.mjpeg"), ("All Files", "*.*")]
        )
        if filepath:
            self.player_load_file(filepath)

    def on_player_select_file(self, event):
        selection = self.player_listbox.curselection()
        if selection:
            key = self.player_listbox.get(selection[0])
            filepath = getattr(self, 'player_file_map', {}).get(key, key)
            if os.path.exists(filepath):
                self.player_load_file(filepath)

    def player_load_file(self, filepath):
        if not os.path.exists(filepath):
            messagebox.showerror("System Screen Error", f"Cannot locate MJPEG: {filepath}")
            return
            
        was_playing = self.player_playing
        self.player_pause()
        
        try:
            self.player_lbl_stats.config(text="CALCULATING MATRIX FRAMES... INDEXING ENTIRE STREAM...")
            self.update_idletasks()
            
            with open(filepath, "rb") as f:
                self.player_file_data = f.read()
                
            self.player_frames = []
            size = len(self.player_file_data)
            pos = 0
            while True:
                start = self.player_file_data.find(b'\xff\xd8', pos)
                if start == -1:
                    break
                end = self.player_file_data.find(b'\xff\xd9', start)
                if end == -1:
                    break
                end += 2
                self.player_frames.append((start, end - start))
                pos = end
                
            if not self.player_frames:
                raise ValueError("Null frame arrays. Byte header parsing returned nothing.")
                
            self.player_filepath = filepath
            self.player_current_frame_idx = 0
            
            filename = os.path.basename(filepath)
            filesize_mb = size / (1024 * 1024)
            self.player_lbl_title.config(text=filename.upper(), foreground=self.color_cyan)
            self.player_lbl_stats.config(text=f"FRAMES: {len(self.player_frames)} | WEIGHT: {filesize_mb:.2f} MB")
            
            self.player_slider.config(to=len(self.player_frames) - 1)
            self.player_lbl_time_total.config(text=f"{len(self.player_frames) - 1:04d}")
            self.player_slider_var.set(0)
            self.player_lbl_time_cur.config(text="0000")
            
            self.player_show_frame(0)
            
            if was_playing:
                self.player_play()
                
        except Exception as e:
            messagebox.showerror("Matrix Parsing Fail", f"Byte indexing error:\n{str(e)}")
            self.player_lbl_title.config(text="INDEX_PARSING_ERROR", foreground=self.color_pink)
            self.player_lbl_stats.config(text="Fail: Index alignment corrupted.")
            self.player_frames = []
            self.player_file_data = None
            self.draw_canvas_grid()

    def player_show_frame(self, index):
        if not self.player_frames or self.player_file_data is None:
            self.draw_canvas_grid()
            return
            
        if index < 0 or index >= len(self.player_frames):
            return
            
        offset, length = self.player_frames[index]
        frame_bytes = self.player_file_data[offset:offset+length]
        
        np_arr = np.frombuffer(frame_bytes, dtype=np.uint8)
        img_bgr = cv2.imdecode(np_arr, cv2.IMREAD_COLOR)
        
        if img_bgr is None:
            return
            
        img_rgb = cv2.cvtColor(img_bgr, cv2.COLOR_BGR2RGB)
        h, w = img_rgb.shape[:2]
        
        new_w = int(w * self.player_scale)
        new_h = int(h * self.player_scale)
        img_resized = cv2.resize(img_rgb, (new_w, new_h))
        
        pil_img = Image.fromarray(img_resized)
        self.player_photo_img = ImageTk.PhotoImage(image=pil_img)
        
        self.player_canvas.delete("all")
        
        canvas_w = self.player_canvas.winfo_width()
        canvas_h = self.player_canvas.winfo_height()
        if canvas_w == 1: canvas_w = new_w
        if canvas_h == 1: canvas_h = new_h
        
        x = max(0, (canvas_w - new_w) // 2)
        y = max(0, (canvas_h - new_h) // 2)
        
        self.player_canvas.create_image(x, y, anchor=tk.NW, image=self.player_photo_img)
        
        # Tech corner accents over active display
        offset_b = 20
        sz_b = 10
        self.player_canvas.create_line(x - offset_b, y - offset_b, x - offset_b + sz_b, y - offset_b, fill=self.color_cyan, width=1)
        self.player_canvas.create_line(x - offset_b, y - offset_b, x - offset_b, y - offset_b + sz_b, fill=self.color_cyan, width=1)
        self.player_canvas.create_line(x + new_w + offset_b, y - offset_b, x + new_w + offset_b - sz_b, y - offset_b, fill=self.color_cyan, width=1)
        self.player_canvas.create_line(x + new_w + offset_b, y - offset_b, x + new_w + offset_b, y - offset_b + sz_b, fill=self.color_cyan, width=1)
        self.player_canvas.create_line(x - offset_b, y + new_h + offset_b, x - offset_b + sz_b, y + new_h + offset_b, fill=self.color_cyan, width=1)
        self.player_canvas.create_line(x - offset_b, y + new_h + offset_b, x - offset_b, y + new_h + offset_b - sz_b, fill=self.color_cyan, width=1)
        self.player_canvas.create_line(x + new_w + offset_b, y + new_h + offset_b, x + new_w + offset_b - sz_b, y + new_h + offset_b, fill=self.color_cyan, width=1)
        self.player_canvas.create_line(x + new_w + offset_b, y + new_h + offset_b, x + new_w + offset_b, y + new_h + offset_b - sz_b, fill=self.color_cyan, width=1)
        
        if not self.player_slider_pressed:
            self.player_slider_var.set(index)
            self.player_lbl_time_cur.config(text=f"{index:04d}")

    def player_update_playback(self):
        if not self.player_playing:
            return
            
        start_time = time.time()
        
        self.player_current_frame_idx += 1
        if self.player_current_frame_idx >= len(self.player_frames):
            if self.player_loop:
                self.player_current_frame_idx = 0
            else:
                self.player_current_frame_idx = len(self.player_frames) - 1
                self.player_pause()
                return
                
        self.player_show_frame(self.player_current_frame_idx)
        
        elapsed = time.time() - start_time
        target_delay = 1.0 / self.player_fps
        sleep_ms = int(max(1, (target_delay - elapsed) * 1000))
        
        self.player_after_id = self.after(sleep_ms, self.player_update_playback)

    def player_toggle_play(self):
        if self.player_playing:
            self.player_pause()
        else:
            self.player_play()

    def player_play(self):
        if not self.player_frames:
            return
        if not self.player_playing:
            self.player_playing = True
            self.player_btn_play.config(text="⏸ PAUSE", fg=self.color_pink, highlightbackground=self.color_pink)
            self.player_update_playback()

    def player_pause(self):
        self.player_playing = False
        self.player_btn_play.config(text="▶ PLAY", fg=self.color_cyan, highlightbackground="#1b2a47")
        if self.player_after_id:
            self.after_cancel(self.player_after_id)
            self.player_after_id = None

    def player_stop(self):
        self.player_pause()
        self.player_current_frame_idx = 0
        self.player_show_frame(0)
        self.player_slider_var.set(0)
        self.player_lbl_time_cur.config(text="0000")

    def player_step_frame(self, direction):
        if not self.player_frames:
            return
        self.player_pause()
        new_idx = self.player_current_frame_idx + direction
        if 0 <= new_idx < len(self.player_frames):
            self.player_current_frame_idx = new_idx
            self.player_show_frame(self.player_current_frame_idx)

    # Slider interactions
    def on_player_slider_press(self, event):
        self.player_slider_pressed = True
        
    def on_player_slider_move(self, value):
        if self.player_slider_pressed and self.player_frames:
            idx = int(float(value))
            if 0 <= idx < len(self.player_frames):
                self.player_current_frame_idx = idx
                self.player_lbl_time_cur.config(text=f"{idx:04d}")
                self.player_show_frame(idx)

    def on_player_slider_release(self, event):
        self.player_slider_pressed = False
        if self.player_frames:
            idx = int(self.player_slider_var.get())
            self.player_current_frame_idx = idx
            self.player_show_frame(idx)

    def on_player_fps_changed(self, event=None):
        try:
            val = int(self.player_fps_var.get())
            if 1 <= val <= 120:
                self.player_fps = val
        except ValueError:
            self.player_fps_var.set(self.player_fps)

    def on_player_scale_changed(self, event):
        val_str = self.player_scale_var.get().replace("%", "")
        try:
            self.player_scale = float(val_str) / 100.0
            self.player_show_frame(self.player_current_frame_idx)
        except ValueError:
            pass

    def player_toggle_loop(self):
        self.player_loop = not self.player_loop

    def on_canvas_resize(self):
        if self.player_file_data is None:
            self.draw_canvas_grid()
        else:
            self.player_show_frame(self.player_current_frame_idx)

    def draw_canvas_grid(self):
        self.player_canvas.delete("all")
        w = self.player_canvas.winfo_width()
        h = self.player_canvas.winfo_height()
        if w <= 1: w = 480
        if h <= 1: h = 600
        
        cx = w // 2
        cy = h // 2
        
        # Grid lines (deep blue-gray)
        grid_step = 40
        for x in range(0, w, grid_step):
            self.player_canvas.create_line(x, 0, x, h, fill="#0c1221", width=1)
        for y in range(0, h, grid_step):
            self.player_canvas.create_line(0, y, w, y, fill="#0c1221", width=1)
            
        # Tactical crosshairs
        self.player_canvas.create_line(cx - 60, cy, cx + 60, cy, fill="#183656", width=1, dash=(4, 4))
        self.player_canvas.create_line(cx, cy - 60, cx, cy + 60, fill="#183656", width=1, dash=(4, 4))
        
        # Circular targeting scopes
        self.player_canvas.create_oval(cx - 30, cy - 30, cx + 30, cy + 30, outline=self.color_cyan, width=1, dash=(2, 2))
        self.player_canvas.create_oval(cx - 90, cy - 90, cx + 90, cy + 90, outline="#102f4a", width=1)
        self.player_canvas.create_oval(cx - 150, cy - 150, cx + 150, cy + 150, outline="#0b1d30", width=1)
        
        # Tactical bracket corner vectors
        offset = 40
        size = 15
        # Top Left
        self.player_canvas.create_line(offset, offset, offset + size, offset, fill=self.color_cyan, width=2)
        self.player_canvas.create_line(offset, offset, offset, offset + size, fill=self.color_cyan, width=2)
        # Top Right
        self.player_canvas.create_line(w - offset, offset, w - offset - size, offset, fill=self.color_cyan, width=2)
        self.player_canvas.create_line(w - offset, offset, w - offset, offset + size, fill=self.color_cyan, width=2)
        # Bottom Left
        self.player_canvas.create_line(offset, h - offset, offset + size, h - offset, fill=self.color_cyan, width=2)
        self.player_canvas.create_line(offset, h - offset, offset, h - offset - size, fill=self.color_cyan, width=2)
        # Bottom Right
        self.player_canvas.create_line(w - offset, h - offset, w - offset - size, h - offset, fill=self.color_cyan, width=2)
        self.player_canvas.create_line(w - offset, h - offset, w - offset, h - offset - size, fill=self.color_cyan, width=2)
        
        # Diagnostic display info
        self.player_canvas.create_text(cx, cy + 130, text="[ DISPLAY_MATRIX_STANDBY ]", fill=self.color_cyan, font=("Consolas", 10, "bold"))
        self.player_canvas.create_text(cx, cy + 152, text="AWAITING VIDEO STREAM FEED...", fill=self.fg_muted, font=("Consolas", 8))

if __name__ == "__main__":
    app = BatchConvertApp()
    app.mainloop()
