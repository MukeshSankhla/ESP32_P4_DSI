import os
import sys
import time
import tkinter as tk
from tkinter import ttk, messagebox, filedialog
import cv2
import numpy as np
from PIL import Image, ImageTk

class MJPEGPlayer(tk.Tk):
    def __init__(self):
        super().__init__()
        
        self.title("High Performance MJPEG Player")
        self.configure(bg="#1e1e1e")
        
        # Player state variables
        self.file_data = None
        self.frames = []  # List of (offset, length)
        self.current_frame_idx = 0
        self.playing = False
        self.fps = 30
        self.loop_playback = True
        self.scale_factor = 0.35  # Default scale factor (will auto-adjust)
        self.after_id = None
        self.current_filepath = None
        
        # Set up GUI theme & styles
        self.setup_styles()
        
        # Build UI layout
        self.build_ui()
        
        # Bind keyboard shortcuts
        self.bind("<space>", lambda e: self.toggle_play())
        self.bind("<Left>", lambda e: self.step_frame(-1))
        self.bind("<Right>", lambda e: self.step_frame(1))
        
        # Auto-detect screen height to adjust scale
        self.update_idletasks()
        screen_height = self.winfo_screenheight()
        target_height = int(screen_height * 0.65)
        self.scale_factor = round(target_height / 1920.0, 2)
        self.scale_var.set(f"{int(self.scale_factor * 100)}%")
        
        # Scan for animations
        self.scan_animations_dir()
        
        # Load the first animation if available
        if self.file_listbox.size() > 0:
            self.file_listbox.selection_set(0)
            self.on_select_file(None)

    def setup_styles(self):
        # Configure ttk styles
        self.style = ttk.Style()
        self.style.theme_use("clam")
        
        # Dark Theme Palette
        self.bg_dark = "#121212"
        self.bg_panel = "#1e1e1e"
        self.bg_control = "#2d2d2d"
        self.fg_light = "#e0e0e0"
        self.fg_accent = "#007acc"
        self.fg_accent_hover = "#1c97ea"
        
        self.style.configure(".", background=self.bg_panel, foreground=self.fg_light)
        self.style.configure("TFrame", background=self.bg_panel)
        self.style.configure("TLabel", background=self.bg_panel, foreground=self.fg_light, font=("Segoe UI", 10))
        self.style.configure("Title.TLabel", font=("Segoe UI", 12, "bold"))
        self.style.configure("Stats.TLabel", font=("Consolas", 9), foreground="#a0a0a0")
        
        self.style.configure("TButton", 
                             background=self.bg_control, 
                             foreground=self.fg_light, 
                             bordercolor="#404040", 
                             relief="flat", 
                             font=("Segoe UI", 9))
        self.style.map("TButton",
                       background=[("active", "#404040"), ("disabled", "#222222")],
                       foreground=[("active", "#ffffff"), ("disabled", "#555555")])
                       
        self.style.configure("Accent.TButton", 
                             background=self.fg_accent, 
                             foreground="#ffffff", 
                             font=("Segoe UI", 9, "bold"))
        self.style.map("Accent.TButton",
                       background=[("active", self.fg_accent_hover)])
                       
        self.style.configure("TCheckbutton", background=self.bg_panel, foreground=self.fg_light)
        self.style.map("TCheckbutton", background=[("active", self.bg_panel)])

    def build_ui(self):
        # Main split container
        self.paned_window = ttk.PanedWindow(self, orient=tk.HORIZONTAL)
        self.paned_window.pack(fill=tk.BOTH, expand=True, padx=5, pady=5)
        
        # Left Panel (Playlist & Files)
        self.left_panel = ttk.Frame(self.paned_window, width=220)
        self.paned_window.add(self.left_panel, weight=0)
        
        # Title for file section
        file_header = ttk.Label(self.left_panel, text="Animations Directory", style="Title.TLabel")
        file_header.pack(anchor=tk.W, pady=(5, 5))
        
        # Listbox for files
        self.listbox_frame = ttk.Frame(self.left_panel)
        self.listbox_frame.pack(fill=tk.BOTH, expand=True)
        
        self.file_listbox = tk.Listbox(
            self.listbox_frame, 
            bg=self.bg_dark, 
            fg=self.fg_light, 
            selectbackground=self.fg_accent, 
            selectforeground="#ffffff", 
            bd=0, 
            highlightthickness=1,
            highlightcolor="#404040",
            highlightbackground="#333333",
            font=("Segoe UI", 9)
        )
        self.file_listbox.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        self.file_listbox.bind("<<ListboxSelect>>", self.on_select_file)
        
        self.scrollbar = ttk.Scrollbar(self.listbox_frame, orient=tk.VERTICAL, command=self.file_listbox.yview)
        self.scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
        self.file_listbox.config(yscrollcommand=self.scrollbar.set)
        
        # File operations buttons
        self.btn_frame = ttk.Frame(self.left_panel)
        self.btn_frame.pack(fill=tk.X, pady=(5, 5))
        
        self.btn_browse = ttk.Button(self.btn_frame, text="📁 Open File...", command=self.browse_file)
        self.btn_browse.pack(fill=tk.X, pady=2)
        
        self.btn_refresh = ttk.Button(self.btn_frame, text="🔄 Refresh List", command=self.scan_animations_dir)
        self.btn_refresh.pack(fill=tk.X, pady=2)
        
        # Right Panel (Video Display & Controls)
        self.right_panel = ttk.Frame(self.paned_window)
        self.paned_window.add(self.right_panel, weight=1)
        
        # Info header
        self.info_frame = ttk.Frame(self.right_panel)
        self.info_frame.pack(fill=tk.X, pady=5)
        
        self.lbl_title = ttk.Label(self.info_frame, text="No File Loaded", style="Title.TLabel")
        self.lbl_title.pack(anchor=tk.W)
        
        self.lbl_stats = ttk.Label(self.info_frame, text="Select or open a video to begin playback.", style="Stats.TLabel")
        self.lbl_stats.pack(anchor=tk.W, pady=(2, 0))
        
        # Video Display Canvas
        self.canvas_container = ttk.Frame(self.right_panel, relief="solid", borderwidth=1)
        self.canvas_container.pack(fill=tk.BOTH, expand=True, pady=5)
        
        # We will use black background for video canvas
        self.canvas = tk.Canvas(self.canvas_container, bg="#080808", bd=0, highlightthickness=0)
        self.canvas.pack(fill=tk.BOTH, expand=True)
        
        # Controls Frame (Bottom)
        self.controls_frame = ttk.Frame(self.right_panel)
        self.controls_frame.pack(fill=tk.X, pady=5)
        
        # 1. Slider & Timeline
        self.timeline_frame = ttk.Frame(self.controls_frame)
        self.timeline_frame.pack(fill=tk.X, pady=(0, 10))
        
        self.lbl_time_cur = ttk.Label(self.timeline_frame, text="0", font=("Consolas", 10))
        self.lbl_time_cur.pack(side=tk.LEFT, padx=5)
        
        self.slider_var = tk.DoubleVar()
        self.slider = ttk.Scale(self.timeline_frame, from_=0, to=100, variable=self.slider_var, command=self.on_slider_move)
        self.slider.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=5)
        self.slider.bind("<ButtonRelease-1>", self.on_slider_release)
        self.slider.bind("<Button-1>", self.on_slider_press)
        
        self.lbl_time_total = ttk.Label(self.timeline_frame, text="0", font=("Consolas", 10))
        self.lbl_time_total.pack(side=tk.RIGHT, padx=5)
        
        # 2. Main buttons row
        self.buttons_frame = ttk.Frame(self.controls_frame)
        self.buttons_frame.pack(fill=tk.X, pady=5)
        
        # Play/Pause
        self.btn_prev = ttk.Button(self.buttons_frame, text="⏮", width=4, command=lambda: self.step_frame(-1))
        self.btn_prev.pack(side=tk.LEFT, padx=2)
        
        self.play_button = ttk.Button(self.buttons_frame, text="▶ Play", style="Accent.TButton", width=10, command=self.toggle_play)
        self.play_button.pack(side=tk.LEFT, padx=2)
        
        self.btn_next = ttk.Button(self.buttons_frame, text="⏭", width=4, command=lambda: self.step_frame(1))
        self.btn_next.pack(side=tk.LEFT, padx=2)
        
        self.btn_stop = ttk.Button(self.buttons_frame, text="⏹ Stop", width=8, command=self.stop)
        self.btn_stop.pack(side=tk.LEFT, padx=5)
        
        # Separator
        ttk.Separator(self.buttons_frame, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=10)
        
        # Speed (FPS) Control
        ttk.Label(self.buttons_frame, text="FPS:").pack(side=tk.LEFT, padx=2)
        self.fps_var = tk.IntVar(value=30)
        self.fps_spin = ttk.Spinbox(self.buttons_frame, from_=1, to=120, width=5, textvariable=self.fps_var, command=self.on_fps_changed)
        self.fps_spin.pack(side=tk.LEFT, padx=2)
        self.fps_spin.bind("<Return>", lambda e: self.on_fps_changed())
        
        # Scale Control
        ttk.Label(self.buttons_frame, text="Scale:").pack(side=tk.LEFT, padx=10)
        self.scale_var = tk.StringVar(value="35%")
        self.scale_combo = ttk.Combobox(self.buttons_frame, values=["15%", "25%", "35%", "50%", "75%", "100%"], textvariable=self.scale_var, width=6, state="readonly")
        self.scale_combo.pack(side=tk.LEFT, padx=2)
        self.scale_combo.bind("<<ComboboxSelected>>", self.on_scale_changed)
        
        # Loop playback
        self.chk_loop = ttk.Checkbutton(self.buttons_frame, text="Loop", variable=tk.BooleanVar(value=True), command=self.toggle_loop)
        self.chk_loop.pack(side=tk.RIGHT, padx=5)
        # Initialize internal variable
        self.loop_playback = True

    def scan_animations_dir(self):
        self.file_listbox.delete(0, tk.END)
        self.file_map = {}
        
        # Potential animation search paths
        repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
        candidate_dirs = [
            "Animations",
            os.path.join(repo_root, "Animations"),
            os.path.join(repo_root, "Animations", "Vertical"),
            os.path.join(repo_root, "Animations", "Horizontal"),
        ]
        
        found_any = False
        for anim_dir in candidate_dirs:
            if os.path.exists(anim_dir) and os.path.isdir(anim_dir):
                mjpeg_files = [f for f in os.listdir(anim_dir) if f.lower().endswith(".mjpeg")]
                if mjpeg_files:
                    found_any = True
                    for f in sorted(mjpeg_files):
                        disp_name = f if anim_dir == "Animations" or anim_dir == os.path.join(repo_root, "Animations") else f"{os.path.basename(anim_dir)}/{f}"
                        if disp_name not in self.file_map:
                            self.file_map[disp_name] = os.path.join(anim_dir, f)
                            self.file_listbox.insert(tk.END, disp_name)
                    
        if not found_any:
            self.lbl_stats.config(text="Animations folder not found. Use Browse to load files.")

    def on_select_file(self, event):
        selection = self.file_listbox.curselection()
        if selection:
            key = self.file_listbox.get(selection[0])
            filepath = getattr(self, 'file_map', {}).get(key, os.path.join("Animations", key))
            if os.path.exists(filepath):
                self.load_file(filepath)

    def browse_file(self):
        filepath = filedialog.askopenfilename(
            title="Open MJPEG Video File",
            filetypes=[("MJPEG Video", "*.mjpeg"), ("All Files", "*.*")]
        )
        if filepath:
            self.load_file(filepath)

    def load_file(self, filepath):
        if not os.path.exists(filepath):
            messagebox.showerror("Error", f"File does not exist: {filepath}")
            return
            
        # Pause any current playback
        was_playing = self.playing
        self.pause()
        
        try:
            self.lbl_stats.config(text="Indexing frames... Please wait...")
            self.update_idletasks()
            
            # Read all binary content and scan for JPEGs
            with open(filepath, "rb") as f:
                self.file_data = f.read()
                
            # Scan for 0xFFD8 to 0xFFD9
            self.frames = []
            size = len(self.file_data)
            pos = 0
            while True:
                start = self.file_data.find(b'\xff\xd8', pos)
                if start == -1:
                    break
                end = self.file_data.find(b'\xff\xd9', start)
                if end == -1:
                    break
                end += 2  # Include EOI marker
                self.frames.append((start, end - start))
                pos = end
                
            if not self.frames:
                raise ValueError("No valid JPEG frames found in this MJPEG file!")
                
            self.current_filepath = filepath
            self.current_frame_idx = 0
            
            # Update info display
            filename = os.path.basename(filepath)
            filesize_mb = size / (1024 * 1024)
            self.lbl_title.config(text=filename)
            self.lbl_stats.config(text=f"Frames: {len(self.frames)} | Size: {filesize_mb:.2f} MB")
            
            # Update timeline slider range
            self.slider.config(to=len(self.frames) - 1)
            self.lbl_time_total.config(text=str(len(self.frames) - 1))
            self.slider_var.set(0)
            self.lbl_time_cur.config(text="0")
            
            # Show first frame
            self.show_frame(0)
            
            if was_playing:
                self.play()
                
        except Exception as e:
            messagebox.showerror("Load Error", f"Failed to load MJPEG file:\n{str(e)}")
            self.lbl_title.config(text="Load Error")
            self.lbl_stats.config(text="Failed to parse file.")
            self.frames = []
            self.file_data = None

    def show_frame(self, index):
        if not self.frames or self.file_data is None:
            return
            
        if index < 0 or index >= len(self.frames):
            return
            
        offset, length = self.frames[index]
        frame_bytes = self.file_data[offset:offset+length]
        
        # Decode JPEG bytes to numpy image
        np_arr = np.frombuffer(frame_bytes, dtype=np.uint8)
        img_bgr = cv2.imdecode(np_arr, cv2.IMREAD_COLOR)
        
        if img_bgr is None:
            return
            
        # Convert BGR to RGB
        img_rgb = cv2.cvtColor(img_bgr, cv2.COLOR_BGR2RGB)
        
        # Get dimensions
        h, w = img_rgb.shape[:2]
        
        # Scale the image
        new_w = int(w * self.scale_factor)
        new_h = int(h * self.scale_factor)
        img_resized = cv2.resize(img_rgb, (new_w, new_h))
        
        # Convert to PhotoImage for Tkinter
        pil_img = Image.fromarray(img_resized)
        self.photo_img = ImageTk.PhotoImage(image=pil_img)
        
        # Draw on Canvas
        self.canvas.delete("all")
        
        # Get canvas current size to center the video
        canvas_w = self.canvas.winfo_width()
        canvas_h = self.canvas.winfo_height()
        if canvas_w == 1: canvas_w = new_w
        if canvas_h == 1: canvas_h = new_h
        
        x = max(0, (canvas_w - new_w) // 2)
        y = max(0, (canvas_h - new_h) // 2)
        
        self.canvas.create_image(x, y, anchor=tk.NW, image=self.photo_img)
        
        # If not dragging, update slider
        if not self.slider_pressed:
            self.slider_var.set(index)
            self.lbl_time_cur.config(text=str(index))

    # Playback loop
    def update_frame(self):
        if not self.playing:
            return
            
        start_time = time.time()
        
        # Move to next frame
        self.current_frame_idx += 1
        if self.current_frame_idx >= len(self.frames):
            if self.loop_playback:
                self.current_frame_idx = 0
            else:
                self.current_frame_idx = len(self.frames) - 1
                self.pause()
                return
                
        self.show_frame(self.current_frame_idx)
        
        # Enforce target FPS
        elapsed = time.time() - start_time
        target_delay = 1.0 / self.fps
        sleep_ms = int(max(1, (target_delay - elapsed) * 1000))
        
        self.after_id = self.after(sleep_ms, self.update_frame)

    def toggle_play(self):
        if self.playing:
            self.pause()
        else:
            self.play()

    def play(self):
        if not self.frames:
            return
        if not self.playing:
            self.playing = True
            self.play_button.config(text="⏸ Pause")
            self.update_frame()

    def pause(self):
        self.playing = False
        self.play_button.config(text="▶ Play")
        if self.after_id:
            self.after_cancel(self.after_id)
            self.after_id = None

    def stop(self):
        self.pause()
        self.current_frame_idx = 0
        self.show_frame(0)
        self.slider_var.set(0)
        self.lbl_time_cur.config(text="0")

    def step_frame(self, direction):
        if not self.frames:
            return
        self.pause()
        new_idx = self.current_frame_idx + direction
        if 0 <= new_idx < len(self.frames):
            self.current_frame_idx = new_idx
            self.show_frame(self.current_frame_idx)

    # Slider interactions
    slider_pressed = False
    
    def on_slider_press(self, event):
        self.slider_pressed = True
        
    def on_slider_move(self, value):
        if self.slider_pressed and self.frames:
            idx = int(float(value))
            if 0 <= idx < len(self.frames):
                self.current_frame_idx = idx
                self.lbl_time_cur.config(text=str(idx))
                self.show_frame(idx)

    def on_slider_release(self, event):
        self.slider_pressed = False
        if self.frames:
            idx = int(self.slider_var.get())
            self.current_frame_idx = idx
            self.show_frame(idx)

    def on_fps_changed(self, event=None):
        try:
            val = int(self.fps_var.get())
            if 1 <= val <= 120:
                self.fps = val
        except ValueError:
            self.fps_var.set(self.fps)

    def on_scale_changed(self, event):
        val_str = self.scale_var.get().replace("%", "")
        try:
            self.scale_factor = float(val_str) / 100.0
            self.show_frame(self.current_frame_idx)
        except ValueError:
            pass

    def toggle_loop(self):
        self.loop_playback = not self.loop_playback

if __name__ == "__main__":
    app = MJPEGPlayer()
    app.geometry("800x800")
    app.mainloop()
