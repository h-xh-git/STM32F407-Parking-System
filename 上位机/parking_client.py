import serial
import serial.tools.list_ports
import threading
import time
import tkinter as tk
import tkinter.font as tkfont
from tkinter import ttk, messagebox, scrolledtext
import struct
import sqlite3
import os
import datetime


# 协议帧接收缓冲上限（字节）：超过该值说明帧头/长度已损坏，需要重新搜索帧头
PROTOCOL_BUFFER_LIMIT = 512


def rounded_rect_points(x1, y1, x2, y2, radius):
    radius = max(0, min(radius, (x2 - x1) / 2, (y2 - y1) / 2))
    return [
        x1 + radius, y1,
        x2 - radius, y1,
        x2, y1,
        x2, y1 + radius,
        x2, y2 - radius,
        x2, y2,
        x2 - radius, y2,
        x1 + radius, y2,
        x1, y2,
        x1, y2 - radius,
        x1, y1 + radius,
        x1, y1,
    ]


class ModernCard(tk.Frame):
    def __init__(self, parent, title, palette, padding=18):
        parent_bg = parent.cget('bg') if hasattr(parent, 'cget') else palette['bg']
        super().__init__(parent, bg=parent_bg, bd=0, highlightthickness=0)
        self.palette = palette
        self.parent_bg = parent_bg
        self.padding = padding
        self.shadow_frame = tk.Frame(self, bg=palette['shadow'], bd=0, highlightthickness=0)
        self.shadow_frame.pack(fill=tk.BOTH, expand=True, padx=(0, 6), pady=(0, 6))

        self.border_frame = tk.Frame(self.shadow_frame, bg=palette['card_border'], bd=0, highlightthickness=0)
        self.border_frame.pack(fill=tk.BOTH, expand=True, padx=(0, 6), pady=(0, 6))

        self.container = tk.Frame(self.border_frame, bg=palette['card_bg'], bd=0, highlightthickness=0)
        self.container.pack(fill=tk.BOTH, expand=True, padx=1, pady=1)
        self.title_label = tk.Label(
            self.container,
            text=title,
            bg=palette['card_bg'],
            fg=palette['text'],
            font=('Microsoft YaHei', 13, 'bold')
        )
        self.title_label.pack(anchor='w', padx=22, pady=(18, 8))

        self.content = tk.Frame(self.container, bg=palette['card_bg'], bd=0, highlightthickness=0)
        self.content.pack(fill=tk.BOTH, expand=True, padx=22, pady=(0, 20))


class ModernButton(tk.Canvas):
    def __init__(self, parent, text, command=None, palette=None, variant='primary', width=None, height=42):
        bg = parent.cget('bg') if hasattr(parent, 'cget') else '#ffffff'
        super().__init__(parent, bg=bg, bd=0, highlightthickness=0, relief='flat', height=height)
        self.palette = palette or {}
        self.command = command
        self.variant = variant
        self.text = text
        self.selected = False
        self.enabled = True
        self.radius = height // 2
        self.font = tkfont.Font(family='Microsoft YaHei', size=10, weight='bold')
        default_width = self.font.measure(text) + 44
        self.min_width = width or max(default_width, 110)
        self.hovered = False

        self.configure(width=self.min_width)
        self.bind('<Configure>', lambda _event: self._redraw())
        self.bind('<Enter>', self._on_enter)
        self.bind('<Leave>', self._on_leave)
        self.bind('<Button-1>', self._on_click)
        self._redraw()

    def _variant_colors(self):
        variants = {
            'primary': (self.palette['primary'], self.palette['primary_hover'], '#ffffff'),
            'secondary': (self.palette['secondary_btn'], self.palette['secondary_hover'], self.palette['text']),
            'danger': (self.palette['danger'], self.palette['danger_hover'], '#ffffff'),
            'success': (self.palette['success'], self.palette['success_hover'], '#ffffff'),
            'sidebar': (self.palette['sidebar_btn'], self.palette['sidebar_hover'], self.palette['sidebar_text']),
            'sidebar-active': (self.palette['sidebar_active'], self.palette['sidebar_active'], '#ffffff'),
        }
        return variants.get(self.variant, variants['primary'])

    def _redraw(self):
        width = max(self.winfo_width(), self.min_width)
        height = max(self.winfo_height(), 42)
        fill, hover_fill, text_color = self._variant_colors()
        bg_fill = hover_fill if self.hovered and self.enabled else fill
        outline = ''

        if self.selected:
            bg_fill = self.palette['sidebar_active']
            text_color = '#ffffff'
        if not self.enabled:
            bg_fill = self.palette['disabled']
            text_color = self.palette['disabled_text']

        self.delete('all')
        self.create_polygon(
            rounded_rect_points(1, 1, width - 1, height - 1, self.radius),
            smooth=True,
            fill=bg_fill,
            outline=outline
        )
        anchor = 'w' if self.variant.startswith('sidebar') else 'center'
        x = 18 if anchor == 'w' else width / 2
        self.create_text(
            x,
            height / 2,
            text=self.text,
            font=self.font,
            fill=text_color,
            anchor=anchor
        )

    def _on_enter(self, _event):
        if self.enabled:
            self.hovered = True
            self._redraw()

    def _on_leave(self, _event):
        if self.hovered:
            self.hovered = False
            self._redraw()

    def _on_click(self, _event):
        if self.enabled and self.command:
            self.command()

    def set_selected(self, selected):
        self.selected = selected
        self._redraw()

    def config(self, **kwargs):
        if 'text' in kwargs:
            self.text = kwargs.pop('text')
        if 'command' in kwargs:
            self.command = kwargs.pop('command')
        if 'state' in kwargs:
            self.enabled = kwargs.pop('state') != tk.DISABLED
        if 'variant' in kwargs:
            self.variant = kwargs.pop('variant')
        super().config(**kwargs)
        self._redraw()

    configure = config


class ModernEntry(tk.Frame):
    def __init__(self, parent, palette, width=16, font=('Microsoft YaHei', 11)):
        super().__init__(parent, bg=palette['card_bg'], bd=0, highlightthickness=0)
        self.palette = palette
        self.border = tk.Frame(self, bg=palette['entry_border'], bd=0, highlightthickness=0)
        self.border.pack(fill=tk.BOTH, expand=True)
        self.entry = tk.Entry(
            self.border,
            width=width,
            font=font,
            relief='flat',
            bd=0,
            bg=palette['entry_bg'],
            fg=palette['text'],
            insertbackground=palette['text'],
            highlightthickness=0
        )
        self.entry.pack(fill=tk.BOTH, expand=True, padx=2, pady=2, ipady=8)
        self.entry.bind('<FocusIn>', lambda _event: self.border.config(bg=palette['entry_focus']))
        self.entry.bind('<FocusOut>', lambda _event: self.border.config(bg=palette['entry_border']))

    def get(self):
        return self.entry.get()

    def insert(self, index, value):
        self.entry.insert(index, value)

    def delete(self, start, end=None):
        self.entry.delete(start, end)

    def bind(self, sequence=None, func=None, add=None):
        return self.entry.bind(sequence, func, add)

    def focus_set(self):
        self.entry.focus_set()

    def config(self, **kwargs):
        bg = kwargs.pop('background', kwargs.pop('bg', None))
        if bg is not None:
            self.entry.config(bg=bg)
        if kwargs:
            self.entry.config(**kwargs)

    configure = config


class BorderedFrame(tk.Frame):
    def __init__(self, parent, bg_color, border_color, padding=1):
        super().__init__(parent, bg=border_color, bd=0, highlightthickness=0)
        self.inner = tk.Frame(self, bg=bg_color, bd=0, highlightthickness=0)
        self.inner.pack(fill=tk.BOTH, expand=True, padx=padding, pady=padding)


class ParkingClient:
    def __init__(self, root):
        self.root = root
        self.root.title("智能停车场系统 - 上位机 v2.0")
        self.root.geometry("1280x840")
        self.root.minsize(1180, 780)
        self.root.resizable(True, True)
        
        self.ser = None
        self.running = False
        self.protocol_buffer = bytearray()

        # 数据库操作锁：串口线程与 Tk 主线程共用同一个 sqlite 连接
        self.db_lock = threading.Lock()
        # 串口写锁：主线程发命令、串口线程回传费用，避免两线程写帧交错
        self.ser_lock = threading.Lock()
        # 列表分帧累积（0x05 记录 / 0x0B 卡片）与结束标记 0x22 的共享状态
        self.accum_lock = threading.Lock()
        self.records_accumulating = False
        self.records_accum = []
        self.records_acc_seq = 0
        self.cards_accumulating = False
        self.cards_accum = []
        self.cards_acc_seq = 0
        
        self.parking_status = {
            'total_spaces': 3,
            'occupied': 0,
            'spaces': [False, False, False]
        }
        self.vehicle_count = 0
        
        self.records = []
        self.cards = []
        
        self.use_local_time = True
        self.current_time = {
            'year': 2024,
            'month': 1,
            'day': 1,
            'hour': 0,
            'minute': 0,
            'second': 0
        }
        self.table_layouts = {}
        
        self.init_styles()
        self.create_ui()
        self.init_database()
        self.refresh_cards_list()
        
        self.start_listener()
        self.start_clock()
        self.start_status_refresh()
    
    def init_styles(self):
        self.style = ttk.Style()
        self.style.theme_use('clam')
        self.colors = {
            'bg': '#eef2ff',
            'content_bg': '#f5f7ff',
            'card_bg': '#ffffff',
            'card_border': '#d9e1f2',
            'shadow': '#dfe6f8',
            'table_header': '#e8ecff',
            'table_row_alt': '#f8faff',
            'table_border': '#dbe4ff',
            'log_bg': '#f8fbff',
            'log_border': '#dbe4ff',
            'text': '#16213e',
            'muted': '#6b7280',
            'primary': '#4f46e5',
            'primary_hover': '#4338ca',
            'secondary_btn': '#e8ecff',
            'secondary_hover': '#dbe4ff',
            'success': '#10b981',
            'success_hover': '#059669',
            'danger': '#ef4444',
            'danger_hover': '#dc2626',
            'warning': '#f59e0b',
            'sidebar_bg': '#0f172a',
            'sidebar_btn': '#0f172a',
            'sidebar_hover': '#1e293b',
            'sidebar_active': '#334155',
            'sidebar_text': '#cbd5e1',
            'topbar_bg': '#111827',
            'topbar_text': '#f8fafc',
            'entry_bg': '#f8fbff',
            'entry_border': '#d0daf0',
            'entry_focus': '#4f46e5',
            'disabled': '#c7d2fe',
            'disabled_text': '#eff6ff',
        }

        self.style.configure('Main.TFrame', background=self.colors['content_bg'])
        self.style.configure('Treeview', font=('Microsoft YaHei', 10), rowheight=34, background='#ffffff', fieldbackground='#ffffff', foreground=self.colors['text'], borderwidth=0)
        self.style.configure('Treeview.Heading', font=('Microsoft YaHei', 10, 'bold'), background=self.colors['table_header'], foreground=self.colors['text'], relief='flat', padding=(8, 8))
        self.style.map('Treeview', background=[('selected', '#dbe4ff')], foreground=[('selected', self.colors['text'])])
        self.style.configure('TRadiobutton', background=self.colors['card_bg'], foreground=self.colors['text'], font=('Microsoft YaHei', 11))
        self.style.map('TRadiobutton', background=[('active', self.colors['card_bg'])])
        self.style.configure('TCheckbutton', background=self.colors['card_bg'], foreground=self.colors['text'], font=('Microsoft YaHei', 11))
        self.style.map('TCheckbutton', background=[('active', self.colors['card_bg'])])
        self.style.configure('TCombobox', fieldbackground='#ffffff', background='#ffffff', foreground=self.colors['text'], borderwidth=0, padding=6)

        self.root.configure(bg=self.colors['bg'])
    
    def create_ui(self):
        self.root.grid_rowconfigure(0, weight=1)
        self.root.grid_rowconfigure(1, weight=0)
        self.root.grid_columnconfigure(0, weight=1)

        self.main_frame = tk.Frame(self.root, bg=self.colors['bg'], bd=0, highlightthickness=0)
        self.main_frame.grid(row=0, column=0, sticky='nsew')
        self.main_frame.grid_rowconfigure(0, weight=1)
        self.main_frame.grid_columnconfigure(0, weight=0)
        self.main_frame.grid_columnconfigure(1, weight=1)

        self.sidebar = tk.Frame(self.main_frame, bg=self.colors['sidebar_bg'], width=220, bd=0, highlightthickness=0)
        self.sidebar.grid(row=0, column=0, sticky='nsew')
        self.sidebar.pack_propagate(False)

        self.content_shell = tk.Frame(self.main_frame, bg=self.colors['content_bg'], bd=0, highlightthickness=0)
        self.content_shell.grid(row=0, column=1, sticky='nsew')
        self.content_shell.grid_rowconfigure(0, weight=0)
        self.content_shell.grid_rowconfigure(1, weight=1)
        self.content_shell.grid_columnconfigure(0, weight=1)

        self.topbar = tk.Frame(self.content_shell, bg=self.colors['topbar_bg'], height=82, bd=0, highlightthickness=0)
        self.topbar.grid(row=0, column=0, sticky='ew', padx=(0, 18), pady=(0, 0))
        self.topbar.pack_propagate(False)

        topbar_left = tk.Frame(self.topbar, bg=self.colors['topbar_bg'])
        topbar_left.pack(side=tk.LEFT, fill=tk.Y, padx=24)
        tk.Label(topbar_left, text="停车场管理中心", bg=self.colors['topbar_bg'], fg='#ffffff', font=('Microsoft YaHei', 16, 'bold')).pack(anchor='w', pady=(16, 0))
        tk.Label(topbar_left, text="保持功能不变的视觉升级版上位机", bg=self.colors['topbar_bg'], fg='#94a3b8', font=('Microsoft YaHei', 10)).pack(anchor='w')

        self.status_indicator = tk.Canvas(self.topbar, width=16, height=16, bg=self.colors['topbar_bg'], bd=0, highlightthickness=0)
        self.status_indicator.pack(side=tk.RIGHT, padx=(0, 12))
        self.status_indicator.create_oval(2, 2, 14, 14, fill=self.colors['danger'], outline='')

        self.topbar_status_label = tk.Label(self.topbar, text="未连接", bg=self.colors['topbar_bg'], fg=self.colors['topbar_text'], font=('Microsoft YaHei', 11, 'bold'))
        self.topbar_status_label.pack(side=tk.RIGHT, padx=(0, 24))

        self.content_frame = tk.Frame(self.content_shell, bg=self.colors['content_bg'], bd=0, highlightthickness=0)
        self.content_frame.grid(row=1, column=0, sticky='nsew', padx=(0, 18), pady=18)

        self.create_sidebar()
        self.create_content()

        self.status_bar = tk.Label(self.root, text="未连接", bg='#0b1220', fg='#e5e7eb', font=('Microsoft YaHei', 10), anchor='w', padx=14, pady=6)
        self.status_bar.grid(row=1, column=0, sticky='ew')
    
    def create_sidebar(self):
        logo_frame = tk.Frame(self.sidebar, bg=self.colors['sidebar_bg'])
        logo_frame.pack(fill=tk.X, padx=18, pady=(24, 18))
        tk.Label(logo_frame, text="停车场系统", bg=self.colors['sidebar_bg'], fg='#ffffff', font=('Microsoft YaHei', 16, 'bold')).pack(anchor='w')
        tk.Label(logo_frame, text="STM32 + RFID 上位机", bg=self.colors['sidebar_bg'], fg='#94a3b8', font=('Microsoft YaHei', 10)).pack(anchor='w', pady=(4, 0))
        
        self.sidebar_buttons = []
        
        btn_monitor = ModernButton(self.sidebar, text="实时监控", command=lambda: self.show_tab('monitor'), palette=self.colors, variant='sidebar', height=46)
        btn_monitor.pack(fill=tk.X, padx=14, pady=4)
        self.sidebar_buttons.append(btn_monitor)
        
        btn_cards = ModernButton(self.sidebar, text="卡片管理", command=lambda: self.show_tab('cards'), palette=self.colors, variant='sidebar', height=46)
        btn_cards.pack(fill=tk.X, padx=14, pady=4)
        self.sidebar_buttons.append(btn_cards)
        
        btn_config = ModernButton(self.sidebar, text="配置模式", command=lambda: self.show_tab('config'), palette=self.colors, variant='sidebar', height=46)
        btn_config.pack(fill=tk.X, padx=14, pady=4)
        self.sidebar_buttons.append(btn_config)
        
        tk.Frame(self.sidebar, bg='#1e293b', height=1).pack(fill=tk.X, padx=18, pady=16)
        
        self.connect_button = ModernButton(self.sidebar, text="连接串口", command=self.show_connect_dialog, palette=self.colors, variant='sidebar', height=44)
        self.connect_button.pack(fill=tk.X, padx=14, pady=4)
        
        self.disconnect_button = ModernButton(self.sidebar, text="断开连接", command=self.close_serial, palette=self.colors, variant='sidebar', height=44)
        self.disconnect_button.pack(fill=tk.X, padx=14, pady=4)

        tk.Label(self.sidebar, text="连接与导航", bg=self.colors['sidebar_bg'], fg='#64748b', font=('Microsoft YaHei', 10)).pack(side=tk.BOTTOM, anchor='w', padx=18, pady=20)
    
    def create_content(self):
        self.monitor_frame = tk.Frame(self.content_frame, bg=self.colors['content_bg'], bd=0, highlightthickness=0)
        self.cards_frame = tk.Frame(self.content_frame, bg=self.colors['content_bg'], bd=0, highlightthickness=0)
        self.config_frame = tk.Frame(self.content_frame, bg=self.colors['content_bg'], bd=0, highlightthickness=0)
        
        self.create_monitor_tab()
        self.create_cards_tab()
        self.create_config_tab()
        
        self.show_tab('monitor')
    
    def show_tab(self, tab_name):
        for frame in [self.monitor_frame, self.cards_frame, self.config_frame]:
            frame.pack_forget()
        
        for btn in self.sidebar_buttons:
            btn.set_selected(False)
        
        if tab_name == 'monitor':
            self.monitor_frame.pack(fill=tk.BOTH, expand=True)
            self.sidebar_buttons[0].set_selected(True)
            self.load_records_from_db()
            if self.ser and self.ser.is_open:
                self.request_records()
        elif tab_name == 'cards':
            self.cards_frame.pack(fill=tk.BOTH, expand=True)
            self.sidebar_buttons[1].set_selected(True)
            self.refresh_cards_list()
            if self.ser and self.ser.is_open:
                self.request_cards()
        elif tab_name == 'config':
            self.config_frame.pack(fill=tk.BOTH, expand=True)
            self.sidebar_buttons[2].set_selected(True)
    
    def create_monitor_tab(self):
        main_grid = tk.Frame(self.monitor_frame, bg=self.colors['content_bg'])
        main_grid.pack(fill=tk.BOTH, expand=True)

        main_grid.grid_columnconfigure(0, weight=1)
        main_grid.grid_columnconfigure(1, weight=1)
        main_grid.grid_rowconfigure(0, weight=0)
        main_grid.grid_rowconfigure(1, weight=1)

        time_card = self.create_card(main_grid, '当前时间')
        time_card.grid(row=0, column=0, sticky='nsew', padx=(0, 15), pady=(0, 15))
        
        self.time_label = tk.Label(time_card.content, text="2024-01-01 00:00:00", bg=self.colors['card_bg'], fg=self.colors['text'], font=('Microsoft YaHei', 24, 'bold'))
        self.time_label.pack(pady=15)
        
        btn_frame = tk.Frame(time_card.content, bg=self.colors['card_bg'])
        btn_frame.pack(pady=(8, 2))
        ModernButton(btn_frame, text="同步时间", command=self.sync_time, palette=self.colors, variant='primary', width=120).pack(side=tk.LEFT, padx=7)
        ModernButton(btn_frame, text="获取时间", command=self.get_time, palette=self.colors, variant='secondary', width=120).pack(side=tk.LEFT, padx=7)
        
        status_card = self.create_card(main_grid, '车位状态')
        status_card.grid(row=0, column=1, sticky='nsew', pady=(0, 15))
        
        status_inner = tk.Frame(status_card.content, bg=self.colors['card_bg'])
        status_inner.pack(fill=tk.BOTH, expand=True, padx=20, pady=15)
        
        stats_row = tk.Frame(status_inner, bg=self.colors['card_bg'])
        stats_row.pack(fill=tk.X, pady=(0, 15))
        
        self.free_display = tk.Label(stats_row, text="3", bg=self.colors['card_bg'], fg=self.colors['success'], font=('Microsoft YaHei', 36, 'bold'))
        self.free_display.pack(side=tk.LEFT)
        
        tk.Label(stats_row, text=" / 3 空闲车位", bg=self.colors['card_bg'], fg=self.colors['muted'], font=('Microsoft YaHei', 14)).pack(side=tk.LEFT, padx=5)
        
        self.space_frames = []
        for i in range(3):
            space_frame = tk.Frame(status_inner, bg=self.colors['card_bg'])
            space_frame.pack(fill=tk.X, pady=5)
            
            status_indicator = tk.Label(space_frame, text="●", bg=self.colors['card_bg'], fg='#10b981', font=('Microsoft YaHei', 18))
            status_indicator.pack(side=tk.LEFT, padx=10)
            
            status_text = tk.Label(space_frame, text=f"车位 {i+1}: 空闲", bg=self.colors['card_bg'], fg='#10b981', font=('Microsoft YaHei', 12))
            status_text.pack(side=tk.LEFT, padx=10)
            
            self.space_frames.append((status_indicator, status_text))
        
        log_card = self.create_card(main_grid, '系统日志')
        log_card.grid(row=1, column=0, sticky='nsew', padx=(0, 15))
        
        log_shell = BorderedFrame(log_card.content, self.colors['log_bg'], self.colors['log_border'])
        log_shell.pack(fill=tk.BOTH, expand=True, padx=10, pady=(10, 8))
        self.log_text = scrolledtext.ScrolledText(
            log_shell.inner,
            font=('Consolas', 10),
            relief='flat',
            bd=0,
            bg=self.colors['log_bg'],
            fg=self.colors['text'],
            insertbackground=self.colors['text'],
            padx=12,
            pady=10,
            spacing1=2,
            spacing3=3
        )
        self.log_text.pack(fill=tk.BOTH, expand=True)
        self.log_text.insert(tk.END, "系统启动...\n")
        
        records_card = self.create_card(main_grid, '车辆记录')
        records_card.grid(row=1, column=1, sticky='nsew')
        records_card.content.grid_columnconfigure(0, weight=1)
        records_card.content.grid_rowconfigure(0, weight=1)

        records_shell, self.records_tree, self.records_scrollbar = self.create_table_view(
            records_card.content,
            ('车牌', '卡号', '入场时间', '出场时间', '费用'),
            ('车牌号', 'RFID卡号', '入场时间', '出场时间', '费用(元)'),
        )
        records_shell.grid(row=0, column=0, sticky='nsew', padx=10, pady=(10, 8))
        self.register_table_layout(self.records_tree, [
            {'id': '车牌', 'min': 110, 'weight': 2},
            {'id': '卡号', 'min': 120, 'weight': 2},
            {'id': '入场时间', 'min': 150, 'weight': 3},
            {'id': '出场时间', 'min': 150, 'weight': 3},
            {'id': '费用', 'min': 90, 'weight': 1},
        ])

        btn_frame2 = tk.Frame(records_card.content, bg=self.colors['card_bg'])
        btn_frame2.grid(row=1, column=0, sticky='ew', padx=10, pady=(4, 2))
        ModernButton(btn_frame2, text="刷新记录", command=self.request_records, palette=self.colors, variant='primary', width=120).pack(side=tk.LEFT, padx=(0, 8))
        ModernButton(btn_frame2, text="清空记录", command=self.clear_records, palette=self.colors, variant='danger', width=120).pack(side=tk.LEFT)

    def create_card(self, parent, title):
        return ModernCard(parent, title, self.colors)

    def create_simple_group(self, parent, title):
        frame = tk.Frame(parent, bg=self.colors['card_bg'], bd=0, highlightthickness=1, highlightbackground=self.colors['card_border'])
        tk.Label(
            frame,
            text=title,
            bg=self.colors['card_bg'],
            fg=self.colors['text'],
            font=('Microsoft YaHei', 12, 'bold')
        ).pack(anchor='w', padx=16, pady=(14, 8))
        content = tk.Frame(frame, bg=self.colors['card_bg'], bd=0, highlightthickness=0)
        content.pack(fill=tk.BOTH, expand=True, padx=16, pady=(0, 14))
        return frame, content

    def create_table_view(self, parent, columns, headings, font_size=11, on_select=None):
        shell = BorderedFrame(parent, '#ffffff', self.colors['table_border'])
        shell.inner.grid_columnconfigure(0, weight=1)
        shell.inner.grid_rowconfigure(0, weight=1)

        tree = ttk.Treeview(shell.inner, columns=columns, show='headings')
        for column_id, heading in zip(columns, headings):
            tree.heading(column_id, text=heading)
            tree.column(column_id, anchor='center')

        tree.grid(row=0, column=0, sticky='nsew')
        scrollbar = ttk.Scrollbar(shell.inner, orient='vertical', command=tree.yview)
        scrollbar.grid(row=0, column=1, sticky='ns')
        tree.configure(yscrollcommand=scrollbar.set)
        if on_select:
            tree.bind('<<TreeviewSelect>>', on_select)

        tree.tag_configure('default', font=('Microsoft YaHei', font_size))
        tree.tag_configure('odd', background='#ffffff')
        tree.tag_configure('even', background=self.colors['table_row_alt'])
        return shell, tree, scrollbar

    def create_stable_button(self, parent, text, command, variant='primary', width=14):
        colors = {
            'primary': (self.colors['primary'], '#ffffff', self.colors['primary_hover']),
            'secondary': (self.colors['secondary_btn'], self.colors['text'], self.colors['secondary_hover']),
            'danger': (self.colors['danger'], '#ffffff', self.colors['danger_hover']),
        }
        bg, fg, active_bg = colors.get(variant, colors['primary'])
        return tk.Button(
            parent,
            text=text,
            command=command,
            font=('Microsoft YaHei', 11, 'bold'),
            width=width,
            height=1,
            bd=0,
            relief='flat',
            bg=bg,
            fg=fg,
            activebackground=active_bg,
            activeforeground=fg,
            highlightthickness=0,
            padx=10,
            pady=8,
            cursor='hand2'
        )

    def create_stat_panel(self, parent, textvariable, subtitle):
        panel = tk.Frame(parent, bg='#f8fbff', bd=0, highlightthickness=1, highlightbackground='#dbe4ff')
        tk.Label(
            panel,
            text="当前状态",
            bg='#f8fbff',
            fg=self.colors['muted'],
            font=('Microsoft YaHei', 10)
        ).pack(pady=(10, 2))
        tk.Label(
            panel,
            textvariable=textvariable,
            bg='#f8fbff',
            fg=self.colors['text'],
            font=('Microsoft YaHei', 18, 'bold')
        ).pack()
        tk.Label(
            panel,
            text=subtitle,
            bg='#f8fbff',
            fg=self.colors['muted'],
            font=('Microsoft YaHei', 10),
            wraplength=360,
            justify='center'
        ).pack(pady=(4, 10), padx=12)
        return panel

    def create_entry_field(self, parent, row, label_text, attr_name, width=16, font=('Microsoft YaHei', 11),
                           unit_text=None, default=None, label_width=None, label_fg=None,
                           label_padx=(0, 14), entry_padx=0, pady=5, entry_sticky='w'):
        tk.Label(
            parent,
            text=label_text,
            bg=self.colors['card_bg'],
            fg=label_fg or self.colors['muted'],
            font=font,
            width=label_width,
            anchor='e'
        ).grid(row=row, column=0, padx=label_padx, pady=pady, sticky='e')

        container = tk.Frame(parent, bg=self.colors['card_bg']) if unit_text else parent
        if unit_text:
            container.grid(row=row, column=1, padx=entry_padx, pady=pady, sticky=entry_sticky)

        entry = ModernEntry(container, self.colors, width=width, font=font)
        if unit_text:
            entry.pack(side=tk.LEFT)
            tk.Label(
                container,
                text=unit_text,
                bg=self.colors['card_bg'],
                fg=self.colors['muted'],
                font=font
            ).pack(side=tk.LEFT, padx=6)
        else:
            entry.grid(row=row, column=1, padx=entry_padx, pady=pady, sticky=entry_sticky)

        if default is not None:
            entry.insert(0, default)

        setattr(self, attr_name, entry)
        return entry

    def register_table_layout(self, treeview, columns):
        self.table_layouts[str(treeview)] = columns
        treeview.bind('<Configure>', lambda event, tv=treeview: self.apply_table_layout(tv), add='+')
        self.root.after_idle(lambda tv=treeview: self.apply_table_layout(tv))

    def apply_table_layout(self, treeview):
        key = str(treeview)
        if key not in self.table_layouts:
            return

        total_width = treeview.winfo_width()
        if total_width <= 40:
            return

        specs = self.table_layouts[key]
        min_total = sum(spec['min'] for spec in specs)
        usable_width = max(total_width - 6, min_total)
        extra = max(usable_width - min_total, 0)
        weight_total = sum(spec['weight'] for spec in specs) or 1

        assigned = 0
        for index, spec in enumerate(specs):
            if index == len(specs) - 1:
                width = usable_width - assigned
            else:
                width = spec['min'] + int(extra * spec['weight'] / weight_total)
                assigned += width
            treeview.column(spec['id'], width=width, minwidth=spec['min'], stretch=False, anchor=spec.get('anchor', 'center'))

    def create_cards_tab(self):
        main_frame = tk.Frame(self.cards_frame, bg=self.colors['content_bg'])
        main_frame.pack(fill=tk.BOTH, expand=True)

        left_panel = tk.Frame(main_frame, bg=self.colors['content_bg'])
        left_panel.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=(0, 15))

        list_card = self.create_card(left_panel, '已绑定卡片列表')
        list_card.pack(fill=tk.BOTH, expand=True)
        list_card.content.grid_columnconfigure(0, weight=1)
        list_card.content.grid_rowconfigure(0, weight=1)

        cards_shell, self.cards_tree, self.cards_scrollbar = self.create_table_view(
            list_card.content,
            ('卡号', '车牌', '余额', '状态'),
            ('RFID卡号', '绑定车牌', '余额(元)', '状态'),
            font_size=10,
            on_select=self.on_card_select,
        )
        cards_shell.grid(row=0, column=0, sticky='nsew', padx=10, pady=(10, 8))
        self.register_table_layout(self.cards_tree, [
            {'id': '卡号', 'min': 150, 'weight': 4},
            {'id': '车牌', 'min': 140, 'weight': 3},
            {'id': '余额', 'min': 90, 'weight': 1},
            {'id': '状态', 'min': 80, 'weight': 1},
        ])

        right_panel = tk.Frame(main_frame, bg=self.colors['content_bg'], width=380)
        right_panel.pack(side=tk.RIGHT, fill=tk.Y)
        right_panel.pack_propagate(False)

        bind_card = self.create_card(right_panel, '卡片绑定 / 更新')
        bind_card.pack(fill=tk.X, pady=(0, 15))

        bind_grid = tk.Frame(bind_card.content, bg=self.colors['card_bg'])
        bind_grid.pack(fill=tk.BOTH, expand=True, padx=20, pady=15)
        bind_grid.columnconfigure(0, weight=1)
        bind_grid.columnconfigure(1, weight=1)

        self.create_entry_field(bind_grid, 0, "卡号 (UID):", 'bind_uid_entry', width=16, label_fg=self.colors['text'], label_padx=10, entry_padx=10, pady=6)
        self.create_entry_field(bind_grid, 1, "车牌号:", 'bind_plate_entry', width=16, label_fg=self.colors['text'], label_padx=10, entry_padx=10, pady=6)
        self.create_entry_field(bind_grid, 2, "余额:", 'bind_balance_entry', width=12, unit_text='元', default='0', label_fg=self.colors['text'], label_padx=10, entry_padx=10, pady=6)

        bind_blacklist_frame = tk.Frame(bind_grid, bg=self.colors['card_bg'])
        bind_blacklist_frame.grid(row=3, column=0, columnspan=2, pady=(6, 4))
        self.bind_blacklist_var = tk.IntVar()
        bind_blacklist_check = tk.Checkbutton(bind_blacklist_frame, text=" 加入黑名单", variable=self.bind_blacklist_var, bg=self.colors['card_bg'], fg='red', selectcolor=self.colors['card_bg'])
        bind_blacklist_check.pack(side=tk.LEFT, padx=(10, 0))

        btn_frame1 = tk.Frame(bind_grid, bg=self.colors['card_bg'])
        btn_frame1.grid(row=4, column=0, columnspan=2, pady=(10, 4), sticky='ew')
        ModernButton(btn_frame1, text="绑定/更新", command=self.bind_card, palette=self.colors, variant='primary', width=138).pack(side=tk.LEFT, padx=(0, 10))
        ModernButton(btn_frame1, text="删除绑定", command=self.delete_card_binding, palette=self.colors, variant='danger', width=138).pack(side=tk.LEFT)
        
        btn_frame1_2 = tk.Frame(bind_grid, bg=self.colors['card_bg'])
        btn_frame1_2.grid(row=5, column=0, columnspan=2, pady=(8, 4), sticky='ew')
        ModernButton(btn_frame1_2, text="设置/解除黑名单", command=self.set_card_blacklist_from_binding, palette=self.colors, variant='warning', width=220).pack()

        recharge_card = self.create_card(right_panel, '卡片充值')
        recharge_card.pack(fill=tk.X, pady=(0, 15))

        recharge_grid = tk.Frame(recharge_card.content, bg=self.colors['card_bg'])
        recharge_grid.pack(fill=tk.BOTH, expand=True, padx=20, pady=15)
        recharge_grid.columnconfigure(0, weight=1)
        recharge_grid.columnconfigure(1, weight=1)

        self.create_entry_field(recharge_grid, 0, "充值金额:", 'recharge_amount_entry', width=12, unit_text='元', label_fg=self.colors['text'], label_padx=10, entry_padx=10, pady=6)

        btn_frame2 = tk.Frame(recharge_grid, bg=self.colors['card_bg'])
        btn_frame2.grid(row=1, column=0, columnspan=2, pady=(14, 4), sticky='ew')
        ModernButton(btn_frame2, text="充值", command=self.recharge_card, palette=self.colors, variant='success', width=138).pack(side=tk.LEFT, padx=(0, 10))
        ModernButton(btn_frame2, text="刷新列表", command=self.refresh_cards_list, palette=self.colors, variant='secondary', width=138).pack(side=tk.LEFT)

        sync_card = self.create_card(right_panel, '数据同步')
        sync_card.pack(fill=tk.X)

        btn_frame3 = tk.Frame(sync_card.content, bg=self.colors['card_bg'])
        btn_frame3.pack(pady=(12, 6))
        ModernButton(btn_frame3, text="同步卡片数据库到STM32", command=self.sync_card_database, palette=self.colors, variant='primary', width=220).pack()

    def create_config_tab(self):
        main_frame = tk.Frame(self.config_frame, bg=self.colors['content_bg'])
        main_frame.pack(fill=tk.BOTH, expand=True)
        main_frame.grid_columnconfigure(0, weight=6)
        main_frame.grid_columnconfigure(1, weight=5)
        main_frame.grid_rowconfigure(0, weight=1)

        left_card = self.create_card(main_frame, '卡片管理')
        left_card.grid(row=0, column=0, sticky='nsew', padx=(0, 15))

        left_inner = tk.Frame(left_card.content, bg=self.colors['card_bg'])
        left_inner.pack(fill=tk.BOTH, expand=True, pady=4, padx=8)

        mode_section, mode_content = self.create_simple_group(left_inner, '模式控制')
        mode_section.pack(fill=tk.X, pady=(0, 8))

        self.config_mode_var = tk.StringVar(value="正常模式")
        mode_center = tk.Frame(mode_content, bg=self.colors['card_bg'])
        mode_center.pack()

        status_panel = self.create_stat_panel(mode_center, self.config_mode_var, "进入配置模式后可对 RC522 卡片进行读写")
        status_panel.pack(fill=tk.X)

        btn_frame = tk.Frame(mode_center, bg=self.colors['card_bg'])
        btn_frame.pack(pady=(10, 4))

        self.enter_btn = self.create_stable_button(btn_frame, "进入配置模式", self.enter_config_mode, variant='primary', width=14)
        self.enter_btn.pack(side=tk.LEFT, padx=(0, 8))

        self.exit_btn = self.create_stable_button(btn_frame, "退出配置模式", self.exit_config_mode, variant='secondary', width=14)
        self.exit_btn.pack(side=tk.LEFT)
        self.exit_btn.config(state=tk.DISABLED)

        self.card_status_label = tk.Label(mode_content, text="请先进入配置模式，然后放置卡片到 RC522", bg=self.colors['card_bg'], fg=self.colors['muted'], font=('Microsoft YaHei', 10))
        self.card_status_label.pack(pady=(4, 0))

        info_section, info_content = self.create_simple_group(left_inner, '卡片信息')
        info_section.pack(fill=tk.X, pady=(0, 8))

        info_grid = tk.Frame(info_content, bg=self.colors['card_bg'])
        info_grid.pack()

        self.create_entry_field(info_grid, 0, "UID", 'uid_entry', width=28, label_width=6)
        self.create_entry_field(info_grid, 1, "车牌号", 'plate_entry', width=28, label_width=6)
        self.create_entry_field(info_grid, 2, "余额", 'balance_entry', width=28, label_width=6)

        blacklist_row = tk.Frame(info_content, bg=self.colors['card_bg'])
        blacklist_row.pack(fill=tk.X, pady=(8, 0))
        tk.Label(blacklist_row, text="黑名单", bg=self.colors['card_bg'], fg=self.colors['muted'], font=('Microsoft YaHei', 11)).pack(side=tk.LEFT)
        self.blacklist_var = tk.BooleanVar()
        ttk.Checkbutton(blacklist_row, variable=self.blacklist_var, text='标记为黑名单').pack(side=tk.RIGHT)

        action_section, action_content = self.create_simple_group(left_inner, '操作')
        action_section.pack(fill=tk.X)

        action_grid = tk.Frame(action_content, bg=self.colors['card_bg'])
        action_grid.pack()

        self.read_btn = self.create_stable_button(action_grid, "读取卡片", self.read_card, variant='secondary', width=16)
        self.read_btn.grid(row=0, column=0, padx=(0, 8), pady=4)
        self.write_btn = self.create_stable_button(action_grid, "写入卡片", self.write_card, variant='primary', width=16)
        self.write_btn.grid(row=0, column=1, padx=(8, 0), pady=4)
        self.set_plate_btn = self.create_stable_button(action_grid, "设置车牌", self.set_plate, variant='secondary', width=16)
        self.set_plate_btn.grid(row=1, column=0, padx=(0, 8), pady=4)
        self.set_balance_btn = self.create_stable_button(action_grid, "设置余额", self.set_balance, variant='secondary', width=16)
        self.set_balance_btn.grid(row=1, column=1, padx=(8, 0), pady=4)
        self.set_blacklist_btn = self.create_stable_button(action_grid, "设置黑名单", self.set_blacklist, variant='danger', width=36)
        self.set_blacklist_btn.grid(row=2, column=0, padx=0, pady=(6, 0), columnspan=2)

        right_card = self.create_card(main_frame, '收费设置')
        right_card.grid(row=0, column=1, sticky='nsew')

        fee_container = tk.Frame(right_card.content, bg=self.colors['card_bg'])
        fee_container.pack(fill=tk.BOTH, expand=True, pady=12, padx=16)

        main_grid = tk.Frame(fee_container, bg=self.colors['card_bg'])
        main_grid.pack(pady=6, padx=10, fill=tk.X)
        main_grid.columnconfigure(0, weight=1)
        main_grid.columnconfigure(1, weight=1)

        tk.Label(main_grid, text="计费规则", bg=self.colors['card_bg'], fg=self.colors['text'], font=('Microsoft YaHei', 12, 'bold')).grid(row=0, column=0, columnspan=2, pady=(0, 10), sticky='w')

        tk.Label(main_grid, text="计费方式:", bg=self.colors['card_bg'], fg=self.colors['text'], font=('Microsoft YaHei', 12)).grid(row=1, column=0, padx=15, pady=6, sticky='e')
        self.fee_unit_var = tk.StringVar(value='hour')
        unit_frame = tk.Frame(main_grid, bg=self.colors['card_bg'])
        unit_frame.grid(row=1, column=1, padx=15, pady=6, sticky='w')
        ttk.Radiobutton(unit_frame, text='按小时', variable=self.fee_unit_var, value='hour').pack(side=tk.LEFT, padx=8)
        ttk.Radiobutton(unit_frame, text='按分钟', variable=self.fee_unit_var, value='minute').pack(side=tk.LEFT, padx=8)

        self.create_entry_field(main_grid, 2, "计费时长:", 'fee_duration_entry', width=8, font=('Microsoft YaHei', 12), unit_text='分钟', default='60', label_fg=self.colors['text'], label_padx=15, entry_padx=15, pady=6)
        self.create_entry_field(main_grid, 3, "收费金额:", 'fee_amount_entry', width=8, font=('Microsoft YaHei', 12), unit_text='元', default='10', label_fg=self.colors['text'], label_padx=15, entry_padx=15, pady=6)

        tk.Frame(main_grid, bg='#e5e7eb', height=1).grid(row=4, column=0, columnspan=2, pady=12, sticky='ew')

        tk.Label(main_grid, text="高级设置", bg=self.colors['card_bg'], fg=self.colors['text'], font=('Microsoft YaHei', 12, 'bold')).grid(row=5, column=0, columnspan=2, pady=(0, 10), sticky='w')

        self.create_entry_field(main_grid, 6, "最低收费:", 'min_fee_entry', width=8, font=('Microsoft YaHei', 12), unit_text='元', default='10', label_fg=self.colors['text'], label_padx=15, entry_padx=15, pady=6)
        self.create_entry_field(main_grid, 7, "免费时长:", 'free_duration_entry', width=8, font=('Microsoft YaHei', 12), unit_text='分钟', default='0', label_fg=self.colors['text'], label_padx=15, entry_padx=15, pady=6)

        btn_section = tk.Frame(fee_container, bg=self.colors['card_bg'])
        btn_section.pack(fill=tk.X, pady=(12, 8))
        btn_section.columnconfigure(0, weight=1)
        btn_section.columnconfigure(1, weight=1)

        self.apply_fee_btn = self.create_stable_button(btn_section, "应用设置", self.set_fee_config, variant='primary', width=14)
        self.apply_fee_btn.grid(row=0, column=0, padx=(10, 8), sticky='ew')
        self.read_fee_btn = self.create_stable_button(btn_section, "读取设置", self.get_fee_config, variant='secondary', width=14)
        self.read_fee_btn.grid(row=0, column=1, padx=(8, 10), sticky='ew')

        info_section = tk.Frame(fee_container, bg='#f8fbff', bd=0, highlightthickness=1, highlightbackground='#dbe4ff')
        info_section.pack(fill=tk.X, pady=(4, 0))

        info_label = tk.Label(info_section, text="计费示例: 按小时 / 60分钟 / 10元 = 每小时收费10元", bg='#f8fbff', fg=self.colors['muted'], font=('Microsoft YaHei', 10))
        info_label.pack(pady=10, padx=15)

        self.original_bg = self.colors['entry_bg']
    def show_connect_dialog(self):
        ConnectDialog(self.root, self.connect_serial, self.colors)

    def set_fee_config(self):
        try:
            fee_unit = 0 if self.fee_unit_var.get() == 'hour' else 1
            fee_duration = int(self.fee_duration_entry.get())
            fee_amount = int(self.fee_amount_entry.get())
            min_fee = int(self.min_fee_entry.get())
            free_duration = int(self.free_duration_entry.get())

            if fee_duration <= 0:
                messagebox.showerror("错误", "计费时长必须大于0")
                return

            if fee_amount <= 0:
                messagebox.showerror("错误", "收费金额必须大于0")
                return

            if min_fee < 0:
                messagebox.showerror("错误", "最低收费不能为负数")
                return

            if free_duration < 0:
                messagebox.showerror("错误", "免费时长不能为负数")
                return

            data = bytearray()
            data.append(fee_unit)
            data.extend(struct.pack('<I', fee_duration))
            data.extend(struct.pack('<I', fee_amount))
            data.extend(struct.pack('<I', min_fee))
            data.extend(struct.pack('<I', free_duration))

            unit_text = '小时' if fee_unit == 0 else '分钟'

            if self.send_packet(0x0A, data):
                messagebox.showinfo(
                    "成功",
                    f"收费设置已更新\n"
                    f"计费方式: 按{unit_text}/{fee_duration}{unit_text}/{fee_amount}元\n"
                    f"最低收费: {min_fee}元\n"
                    f"免费时长: {free_duration}分钟"
                )
                self.log(f"收费设置更新: 按{unit_text}/{fee_duration}{unit_text}/{fee_amount}元, 最低{min_fee}元, 免费{free_duration}分钟")
            else:
                messagebox.showerror("错误", "发送失败，请检查串口连接")
        except ValueError:
            messagebox.showerror("错误", "请输入有效数字")
    def get_fee_config(self):
        if self.send_packet(0x23):
            self.log("已发送读取收费设置请求")
        else:
            messagebox.showerror("错误", "发送失败，请检查串口连接")
    
    def bind_card(self):
        uid = self.bind_uid_entry.get().strip().upper()
        plate = self.bind_plate_entry.get().strip().upper()
        
        if not uid or len(uid) != 8:
            messagebox.showerror("错误", "请输入8位十六进制UID")
            return
        
        if not plate:
            messagebox.showerror("错误", "请输入车牌号")
            return
        
        try:
            bytes.fromhex(uid)
        except:
            messagebox.showerror("错误", "UID必须是8位十六进制")
            return
        
        try:
            balance = int(self.bind_balance_entry.get() or '0')
            if balance < 0:
                messagebox.showerror("错误", "余额不能为负数")
                return
        except ValueError:
            messagebox.showerror("错误", "余额必须是整数")
            return
        
        blacklist = self.bind_blacklist_var.get()
        
        if self.add_card(uid, plate, balance):
            with self.db_lock:
                self.db_cursor.execute(
                    "UPDATE cards SET blacklist=?, updated_at=CURRENT_TIMESTAMP WHERE uid=?",
                    (blacklist, uid)
                )
                self.db_conn.commit()
            
            messagebox.showinfo("成功", f"卡片 {uid} 已绑定车牌 {plate}，余额: {balance}元")
            self.bind_uid_entry.delete(0, tk.END)
            self.bind_plate_entry.delete(0, tk.END)
            self.bind_balance_entry.delete(0, tk.END)
            self.bind_balance_entry.insert(0, '0')
            self.bind_blacklist_var.set(0)
            self.refresh_cards_list()
            self.log(f"卡片绑定: UID={uid}, 车牌={plate}, 余额={balance}, 黑名单={blacklist}")
        else:
            messagebox.showerror("错误", "绑定失败")
    
    def refresh_cards_list(self):
        if not hasattr(self, 'db_cursor'):
            return
        
        for item in self.cards_tree.get_children():
            self.cards_tree.delete(item)
        
        cards = self.get_all_cards()
        if not cards:
            self.cards_tree.insert('', tk.END, values=('暂无卡片', '-', '-', '-'), tags=('odd',))
            return

        for index, card in enumerate(cards):
            uid, plate, balance, blacklist, created, updated = card
            status = '黑名单' if blacklist else '正常'
            
            tag = 'even' if index % 2 else 'odd'
            self.cards_tree.insert('', tk.END, values=(uid, plate, balance, status), tags=(tag,))
    
    def on_card_select(self, event):
        selected = self.cards_tree.selection()
        if selected:
            item = self.cards_tree.item(selected[0])
            values = item['values']
            if values:
                self.bind_uid_entry.delete(0, tk.END)
                self.bind_uid_entry.insert(0, values[0])
                self.bind_plate_entry.delete(0, tk.END)
                self.bind_plate_entry.insert(0, values[1])
                if len(values) > 2:
                    self.bind_balance_entry.delete(0, tk.END)
                    self.bind_balance_entry.insert(0, str(values[2]))
    
    def recharge_card(self):
        selected = self.cards_tree.selection()
        if not selected:
            messagebox.showwarning("警告", "请选择要充值的卡片")
            return
        
        item = self.cards_tree.item(selected[0])
        uid = item['values'][0]
        plate = item['values'][1]
        
        try:
            amount = int(self.recharge_amount_entry.get())
            if amount <= 0:
                messagebox.showerror("错误", "充值金额必须大于0")
                return
        except ValueError:
            messagebox.showerror("错误", "充值金额必须是整数")
            return
        
        card_info = self.get_card_by_uid(uid)
        if card_info:
            current_balance = card_info[2]
            new_balance = current_balance + amount
            
            if self.update_card_balance(uid, new_balance):
                messagebox.showinfo("成功", f"卡片 {uid} ({plate}) 充值成功！\n充值金额: {amount}元\n当前余额: {new_balance}元")
                self.recharge_amount_entry.delete(0, tk.END)
                self.refresh_cards_list()
                self.log(f"卡片充值: UID={uid}, 车牌={plate}, 充值金额={amount}, 新余额={new_balance}")
            else:
                messagebox.showerror("错误", "充值失败")
        else:
            messagebox.showerror("错误", "未找到该卡片")
    
    def delete_card_binding(self):
        selected = self.cards_tree.selection()
        if not selected:
            messagebox.showwarning("警告", "请选择要删除的卡片")
            return
        
        item = self.cards_tree.item(selected[0])
        uid = item['values'][0]
        
        if messagebox.askyesno("确认", f"确定删除卡片 {uid} 的绑定?"):
            if self.delete_card_by_uid(uid):
                messagebox.showinfo("成功", f"卡片 {uid} 已解绑")
                self.refresh_cards_list()
                self.log(f"卡片解绑: UID={uid}")
    
    def init_database(self):
        db_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'parking.db')
        self.db_conn = sqlite3.connect(db_path, check_same_thread=False)
        self.db_cursor = self.db_conn.cursor()
        
        with self.db_lock:
            self.db_cursor.execute('''
                CREATE TABLE IF NOT EXISTS cards (
                    uid TEXT PRIMARY KEY,
                    plate TEXT NOT NULL,
                    balance INTEGER DEFAULT 0,
                    blacklist INTEGER DEFAULT 0,
                    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
                    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
                )
            ''')
            
            self.db_cursor.execute('''
                CREATE TABLE IF NOT EXISTS vehicle_records (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    uid TEXT,
                    plate TEXT,
                    in_time TEXT,
                    out_time TEXT,
                    fee INTEGER DEFAULT 0,
                    status TEXT,
                    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
                )
            ''')
            
            self.db_conn.commit()
        self.log(f"数据库初始化成功: {db_path}")
    
    def get_card_by_uid(self, uid):
        with self.db_lock:
            self.db_cursor.execute("SELECT * FROM cards WHERE uid=?", (uid,))
            return self.db_cursor.fetchone()
    
    def add_card(self, uid, plate, balance=0):
        with self.db_lock:
            try:
                self.db_cursor.execute(
                    "INSERT INTO cards (uid, plate, balance) VALUES (?, ?, ?)",
                    (uid, plate, balance)
                )
                self.db_conn.commit()
                return True
            except sqlite3.IntegrityError:
                self.db_cursor.execute(
                    "UPDATE cards SET plate=?, balance=?, updated_at=CURRENT_TIMESTAMP WHERE uid=?",
                    (plate, balance, uid)
                )
                self.db_conn.commit()
                return True
        return False

    def sync_card_to_db(self, uid, plate, balance=0, blacklist=0):
        with self.db_lock:
            self.db_cursor.execute(
                """
                INSERT INTO cards (uid, plate, balance, blacklist)
                VALUES (?, ?, ?, ?)
                ON CONFLICT(uid) DO UPDATE SET
                    plate=excluded.plate,
                    balance=excluded.balance,
                    blacklist=excluded.blacklist,
                    updated_at=CURRENT_TIMESTAMP
                """,
                (uid, plate, balance, blacklist)
            )
            self.db_conn.commit()
    
    def update_card_plate(self, uid, plate):
        with self.db_lock:
            self.db_cursor.execute(
                "UPDATE cards SET plate=?, updated_at=CURRENT_TIMESTAMP WHERE uid=?",
                (plate, uid)
            )
            self.db_conn.commit()
            return self.db_cursor.rowcount > 0
    
    def update_card_balance(self, uid, balance):
        with self.db_lock:
            self.db_cursor.execute(
                "UPDATE cards SET balance=?, updated_at=CURRENT_TIMESTAMP WHERE uid=?",
                (balance, uid)
            )
            self.db_conn.commit()
            return self.db_cursor.rowcount > 0
    
    def delete_card_by_uid(self, uid):
        with self.db_lock:
            self.db_cursor.execute("DELETE FROM cards WHERE uid=?", (uid,))
            self.db_conn.commit()
            return self.db_cursor.rowcount > 0
    
    def get_all_cards(self):
        with self.db_lock:
            self.db_cursor.execute("SELECT * FROM cards ORDER BY created_at DESC")
            return self.db_cursor.fetchall()
    
    def add_vehicle_record(self, uid, plate, in_time, status='在场'):
        with self.db_lock:
            self.db_cursor.execute(
                "INSERT INTO vehicle_records (uid, plate, in_time, status) VALUES (?, ?, ?, ?)",
                (uid, plate, in_time, status)
            )
            self.db_conn.commit()
            return self.db_cursor.lastrowid
    
    def update_vehicle_record(self, uid, out_time, fee):
        with self.db_lock:
            self.db_cursor.execute(
                "UPDATE vehicle_records SET out_time=?, fee=?, status='已离场' WHERE uid=? AND status='在场'",
                (out_time, fee, uid)
            )
            self.db_conn.commit()
            return self.db_cursor.rowcount > 0
    
    def get_vehicle_records(self):
        with self.db_lock:
            self.db_cursor.execute("SELECT * FROM vehicle_records ORDER BY created_at DESC LIMIT 50")
            return self.db_cursor.fetchall()

    def load_records_from_db(self):
        if not hasattr(self, 'db_cursor'):
            return

        db_records = self.get_vehicle_records()
        self.records = []
        for record in db_records:
            _, uid, plate, in_time, out_time, fee, status, _created_at = record
            self.records.append({
                'plate': plate or '-',
                'uid': uid or '-',
                'in_time': in_time or '-',
                'out_time': out_time or '-',
                'fee': fee if fee is not None else 0,
                'status': status or '-'
            })
        if hasattr(self, 'records_tree'):
            self.update_records_display()
    
    def clear_vehicle_records(self):
        with self.db_lock:
            self.db_cursor.execute("DELETE FROM vehicle_records")
            self.db_conn.commit()
    
    def start_listener(self):
        self.listener_thread = threading.Thread(target=self.serial_listener, daemon=True)
        self.listener_thread.start()
    
    def serial_listener(self):
        while True:
            if self.ser and self.ser.is_open:
                try:
                    # 一次读出当前可用的全部字节，减少 1ms 空转
                    waiting = self.ser.in_waiting
                    data = self.ser.read(waiting if waiting > 0 else 1)
                    if data:
                        self.process_bytes(data)
                except Exception as e:
                    self.log(f"串口读取错误: {e}")
                    self.close_serial()
            time.sleep(0.001)
    
    def process_bytes(self, data):
        """串口线程：把一段接收数据按小块写入缓冲并解析（缓冲上限 512 字节）。"""
        if not data:
            return
        for offset in range(0, len(data), 256):
            self.protocol_buffer.extend(data[offset:offset + 256])
            self._process_protocol_buffer()
            if len(self.protocol_buffer) > PROTOCOL_BUFFER_LIMIT:
                # 帧头/长度损坏导致无法成帧：只保留尾部两字节，重新等待帧头
                del self.protocol_buffer[:len(self.protocol_buffer) - 2]
                self._process_protocol_buffer()

    def process_byte(self, byte):
        """单字节入口（调试/兼容用），与整块接收共用同一分帧状态机。"""
        self.protocol_buffer.append(byte)
        self._process_protocol_buffer()

    def _process_protocol_buffer(self):
        """协议分帧状态机：搜索 AA 55 -> 校验 -> 派发；异常时重新同步。"""
        while True:
            buf = self.protocol_buffer
            if len(buf) < 2:
                return

            # 1) 重同步：缓冲头不是 AA 55 时丢弃垃圾字节，搜索下一个帧头
            if buf[0] != 0xAA or buf[1] != 0x55:
                index = buf.find(b'\xAA\x55', 1)
                if index < 0:
                    # 末尾可能是下一个帧头的第一个字节，保留
                    if buf[-1] == 0xAA:
                        self.protocol_buffer = bytearray(b'\xAA')
                    else:
                        self.protocol_buffer = bytearray()
                    return
                del buf[:index]
                continue

            if len(buf) < 4:
                return

            cmd = buf[2]
            length = buf[3]
            frame_len = 5 + length

            # 2) 缓冲超限：长度字段与实际数据严重不符，丢弃帧头重新搜索
            if len(buf) > PROTOCOL_BUFFER_LIMIT:
                del buf[:1]
                continue

            if len(buf) < frame_len:
                return

            checksum = buf[4 + length]
            calculated = sum(buf[2:4 + length]) & 0xFF

            # 3) 校验失败：丢弃当前帧首字节，重新搜索下一个 AA 55
            if checksum != calculated:
                self.root.after(0, self._debug_log,
                                f"校验和错误: 期望=0x{checksum:02X}, 计算=0x{calculated:02X}")
                del buf[:1]
                continue

            data = bytes(buf[4:4 + length])
            del buf[:frame_len]

            # 4) 派发：handler 异常不能导致串口线程退出
            try:
                self.handle_packet(cmd, data)
            except Exception as e:
                self.root.after(0, self._debug_log, f"数据包处理异常: cmd=0x{cmd:02X}, {e}")

    # ==================== 多帧列表累积（0x05 记录 / 0x0B 卡片） ====================
    def _records_append(self, records_data):
        """串口线程：把一帧记录并入累积缓冲；首帧启动 1500ms 兜底定时器。"""
        with self.accum_lock:
            first_frame = not self.records_accumulating
            if first_frame:
                self.records_accum = []
                self.records_accumulating = True
                self.records_acc_seq += 1
                seq = self.records_acc_seq
            self.records_accum.extend(records_data)
        if first_frame:
            # 兼容旧固件（单帧、无 0x22 结束标记）：1500ms 未收到结束标记则兜底刷新
            self.root.after(1500, self._records_flush_timeout, seq)

    def _records_flush(self, expect_data=True):
        """串口线程：结束记录累积并刷新 UI；expect_data=False 时只刷新挂起的数据。"""
        with self.accum_lock:
            if not self.records_accumulating and not expect_data:
                return
            records_data = self.records_accum
            self.records_accum = []
            self.records_accumulating = False
            self.records_acc_seq += 1
        self.root.after(0, self._handle_records_ui, records_data)

    def _records_flush_timeout(self, seq):
        """Tk 主线程：旧固件无结束标记时，1500ms 兜底刷新一次。"""
        with self.accum_lock:
            if seq != self.records_acc_seq or not self.records_accumulating:
                return
            records_data = self.records_accum
            self.records_accum = []
            self.records_accumulating = False
            self.records_acc_seq += 1
        self._log_ui("记录列表未收到 0x22 结束标记，按超时兜底刷新")
        self._handle_records_ui(records_data)

    def _cards_append(self, cards_data):
        """串口线程：把一帧卡片并入累积缓冲；首帧启动 1500ms 兜底定时器。"""
        with self.accum_lock:
            first_frame = not self.cards_accumulating
            if first_frame:
                self.cards_accum = []
                self.cards_accumulating = True
                self.cards_acc_seq += 1
                seq = self.cards_acc_seq
            self.cards_accum.extend(cards_data)
        if first_frame:
            self.root.after(1500, self._cards_flush_timeout, seq)

    def _cards_flush(self, expect_data=True):
        """串口线程：结束卡片累积并刷新 UI（落库在 _handle_cards_ui 中完成）。"""
        with self.accum_lock:
            if not self.cards_accumulating and not expect_data:
                return
            cards_data = self.cards_accum
            self.cards_accum = []
            self.cards_accumulating = False
            self.cards_acc_seq += 1
        self.root.after(0, self._handle_cards_ui, cards_data)

    def _cards_flush_timeout(self, seq):
        """Tk 主线程：旧固件无结束标记时，1500ms 兜底刷新一次。"""
        with self.accum_lock:
            if seq != self.cards_acc_seq or not self.cards_accumulating:
                return
            cards_data = self.cards_accum
            self.cards_accum = []
            self.cards_accumulating = False
            self.cards_acc_seq += 1
        self._log_ui("卡片列表未收到 0x22 结束标记，按超时兜底刷新")
        self._handle_cards_ui(cards_data)
    
    def _debug_log(self, msg):
        self.log(msg)
    
    def handle_packet(self, cmd, data):
        if cmd == 0x80:
            if len(data) < 2:
                self._debug_log(f"应答包长度异常: {len(data)}")
                return
            sub_cmd = data[0]
            result = data[1]
            
            if sub_cmd == 0x13:
                self.show_card_status("✓ 卡片写入成功", 'green')
            elif sub_cmd == 0x14:
                self.show_card_status("✓ 车牌设置成功", 'green')
            elif sub_cmd == 0x15:
                self.show_card_status("✓ 余额设置成功", 'green')
            elif sub_cmd == 0x16:
                self.show_card_status("✓ 黑名单设置成功", 'green')
            
            return

        elif cmd == 0x81:
            # 固件返回错误：payload = [原命令, 错误码]
            if len(data) >= 2:
                self.log(f"固件返回错误: cmd=0x{data[0]:02X}, result={data[1]}")
            return

        elif cmd == 0x22:
            self.handle_sync_ack(data)
        
        elif cmd == 0x02:
            self.log("收到车辆入场数据包")
            self.handle_parking_in(data)
        
        elif cmd == 0x03:
            self.log("收到车辆出场数据包")
            self.handle_parking_out(data)
        
        elif cmd == 0x04:
            self.handle_status(data)
        
        elif cmd == 0x05:
            self.handle_records(data)
        
        elif cmd == 0x0B:
            self.handle_cards(data)
        
        elif cmd == 0x12:
            self.handle_card_data(data)
        
        elif cmd == 0x21:
            self.handle_time(data)
        
        else:
            # 不显示未知命令的日志
            pass

    def handle_sync_ack(self, data):
        """0x22 同步确认：0x02=记录列表结束、0x03=卡片列表结束、0x01=全量同步结束。"""
        if not data:
            self.log("同步确认包为空")
            return
        sync_type = data[0]
        if sync_type == 0x02:
            self._records_flush()
        elif sync_type == 0x03:
            self._cards_flush()
        elif sync_type == 0x01:
            # 全量同步的最后一条：只刷新仍在累积的数据，避免把已有列表清空
            self._records_flush(expect_data=False)
            self._cards_flush(expect_data=False)
            self.root.after(0, self._log_ui, "全量数据同步完成")
        else:
            self.log(f"未知同步确认类型: 0x{sync_type:02X}")
    
    def decode_plate(self, data):
        """尝试多种编码方式解码车牌"""
        if isinstance(data, bytes):
            try:
                result = data.decode('utf-8').strip('\x00').strip()
                if result:
                    return result
            except:
                pass
            try:
                return data.decode('gbk', errors='ignore').strip('\x00').strip()
            except:
                pass
        return str(data).strip('\x00').strip()
    
    def handle_parking_in(self, data):
        # 最小长度：车位(1) + UID(4) + 车牌(16) + 时间戳(4) = 25
        if len(data) < 25:
            self._debug_log(f"入场数据包长度不足: {len(data)}")
            return
        space_id = data[0]
        uid = data[1:5]
        plate_from_stm = self.decode_plate(data[5:21])
        timestamp = struct.unpack('<I', data[21:25])[0]
        
        time_str = self.timestamp_to_str(timestamp)
        uid_str = ''.join(f'{b:02X}' for b in uid)
        
        card_info = self.get_card_by_uid(uid_str)
        if card_info:
            plate = card_info[1]
        else:
            plate = plate_from_stm
        
        self.root.after(0, self._handle_parking_in_ui, space_id, plate, uid_str, time_str)
    
    def _handle_parking_in_ui(self, space_id, plate, uid_str, time_str):
        self.log(f"车辆入场: 车位{space_id+1}, 车牌={plate}, 卡号={uid_str}, 时间={time_str}")
        
        # 防御固件异常数据：车位编号越界时按 0 号车位处理，避免 IndexError
        if not 0 <= space_id < len(self.parking_status['spaces']):
            self._log_ui(f"车位编号越界: {space_id}，按 0 号车位记录")
            space_id = 0
        self.parking_status['spaces'][space_id] = True
        self.vehicle_count = min(self.vehicle_count + 1, 3)
        self.update_space_display()
        
        self.add_vehicle_record(uid_str, plate, time_str)
        
        self.records.insert(0, {
            'plate': plate,
            'uid': uid_str,
            'in_time': time_str,
            'out_time': '-',
            'fee': 0,
            'status': '在场'
        })
        self.update_records_display()
    
    def calculate_fee(self, in_time, out_time):
        """按秒级时间戳（epoch 2000）计算停车费。

        fee_duration = 计费周期（分钟）、fee_amount = 每周期金额、
        min_fee = 最低消费、free_duration = 免费分钟数。
        不足一个周期按一个周期计（向上取整）；异常时兜底 5 元。
        """
        try:
            fee_duration = int(self.fee_duration_entry.get())
            fee_amount = int(self.fee_amount_entry.get())
            min_fee = int(self.min_fee_entry.get())
            free_duration = int(self.free_duration_entry.get())

            parking_duration = out_time - in_time
            if parking_duration <= 0:
                return 0

            parking_minutes = parking_duration // 60
            if free_duration > 0 and parking_minutes <= free_duration:
                return 0

            actual_minutes = max(parking_minutes - free_duration, 1)
            if fee_duration <= 0:
                fee_duration = 1

            # 向上取整：不足一个计费周期也按一个周期收费
            units = (actual_minutes + fee_duration - 1) // fee_duration
            if units == 0:
                units = 1

            return max(units * fee_amount, min_fee)
        except Exception as e:
            self.log(f"费用计算错误: {e}")
            return 5

    def handle_parking_out(self, data):
        # 最小长度：UID(4) + 车牌(16) + 入场时间(4) + 出场时间(4) = 28
        if len(data) < 28:
            self._debug_log(f"出场数据包长度不足: {len(data)}")
            return
        uid = data[0:4]
        plate_from_stm = self.decode_plate(data[4:20])
        in_time = struct.unpack('<I', data[20:24])[0]
        out_time = struct.unpack('<I', data[24:28])[0]
        
        uid_str = ''.join(f'{b:02X}' for b in uid)
        in_time_str = self.timestamp_to_str(in_time)
        out_time_str = self.timestamp_to_str(out_time)
        
        fee = self.calculate_fee(in_time, out_time)
        
        card_info = self.get_card_by_uid(uid_str)
        if card_info:
            plate = card_info[1]
            current_balance = card_info[2]
            new_balance = current_balance - fee
            self.update_card_balance(uid_str, new_balance)
        else:
            plate = plate_from_stm
            current_balance = 0
            new_balance = 0
        
        self.update_vehicle_record(uid_str, out_time_str, fee)
        
        self.send_parking_out_fee(fee)
        
        self.root.after(0, self._handle_parking_out_ui, plate, uid_str, in_time_str, out_time_str, fee, current_balance, new_balance)
    
    def send_parking_out_fee(self, fee):
        cmd = 0x24
        fee_bytes = struct.pack('<I', fee)
        checksum = cmd + len(fee_bytes)
        for b in fee_bytes:
            checksum += b
        packet = bytes([0xAA, 0x55, cmd, len(fee_bytes)]) + fee_bytes + bytes([checksum & 0xFF])
        if self.ser and self.ser.is_open:
            try:
                with self.ser_lock:
                    self.ser.write(packet)
                self.log(f"发送费用回传: fee={fee}")
            except Exception as e:
                self.log(f"发送费用回传失败: {e}")
    
    def _handle_parking_out_ui(self, plate, uid_str, in_time_str, out_time_str, fee, current_balance, new_balance):
        if current_balance > 0:
            self.log(f"余额更新: UID={uid_str}, 原余额={current_balance}, 扣除={fee}, 新余额={new_balance}")
        
        self.log(f"车辆出场: 车牌={plate}, 卡号={uid_str}, 入场={in_time_str}, 出场={out_time_str}, 费用={fee}元")
        
        for i, rec in enumerate(self.records):
            if rec['uid'] == uid_str and rec['status'] == '在场':
                self.records[i]['out_time'] = out_time_str
                self.records[i]['fee'] = fee
                self.records[i]['status'] = '已离场'
                break
        
        for i in range(3):
            if self.parking_status['spaces'][i]:
                self.parking_status['spaces'][i] = False
                break
        
        self.vehicle_count = max(self.vehicle_count - 1, 0)
        self.update_space_display()
        self.update_records_display()
        self.refresh_cards_list()
    
    def handle_status(self, data):
        # 最小长度：车位总数(1) + 占用数(1) + 3 路车位状态(3) = 5，协议实际发 6，按 6 校验
        if len(data) < 6:
            self._debug_log(f"状态数据包长度不足: {len(data)}")
            return
        total_spaces = data[0]
        occupied = data[1]
        spaces = [bool(data[2+i]) for i in range(3)]
        self.root.after(0, self._handle_status_ui, total_spaces, occupied, spaces)
    
    def _handle_status_ui(self, total_spaces, occupied, spaces):
        self.parking_status['total_spaces'] = total_spaces
        self.parking_status['occupied'] = occupied
        for i in range(3):
            self.parking_status['spaces'][i] = spaces[i]
        self.update_space_display()
    
    def handle_records(self, data):
        # 记录列表分帧传输：每帧 payload 必须是 33 字节记录的整数倍，
        # 多帧累积到 0x22 结束标记（[0x02,0x00]）后再一次性刷新 UI。
        if len(data) % 33 != 0:
            self._debug_log(f"记录数据长度异常: {len(data)} 不是 33 的整数倍")
            return

        records_data = []
        for i in range(0, len(data), 33):
            uid = data[i:i+4]
            plate_from_stm = self.decode_plate(data[i+4:i+20])
            in_time = struct.unpack('<I', data[i+20:i+24])[0]
            out_time = struct.unpack('<I', data[i+24:i+28])[0]
            fee = struct.unpack('<I', data[i+28:i+32])[0]
            status = data[i+32]
            
            uid_str = ''.join(f'{b:02X}' for b in uid)
            in_time_str = self.timestamp_to_str(in_time)
            out_time_str = self.timestamp_to_str(out_time) if out_time != 0 else '-'
            
            card_info = self.get_card_by_uid(uid_str)
            if card_info:
                plate = card_info[1]
            else:
                plate = plate_from_stm
            
            records_data.append({
                'plate': plate,
                'uid': uid_str,
                'in_time': in_time_str,
                'out_time': out_time_str,
                'fee': fee,
                'status': '已离场' if status == 2 else '在场'
            })
        
        self._records_append(records_data)
    
    def _handle_records_ui(self, records_data):
        self.records = records_data
        self.update_records_display()
    
    def handle_cards(self, data):
        # 卡片列表分帧传输：每帧 payload 必须是 29 字节卡片的整数倍，
        # 多帧累积到 0x22 结束标记（[0x03,0x00]）后再刷新 UI 与落库。
        if len(data) % 29 != 0:
            self._debug_log(f"卡片数据长度异常: {len(data)} 不是 29 的整数倍")
            return

        cards_data = []
        for i in range(0, len(data), 29):
            uid = data[i:i+4]
            plate = self.decode_plate(data[i+4:i+20])
            balance = struct.unpack('<I', data[i+20:i+24])[0]
            blacklist = data[i+24]
            valid_date = struct.unpack('<I', data[i+25:i+29])[0]
            
            uid_str = ''.join(f'{b:02X}' for b in uid)
            valid_date_str = self.timestamp_to_str(valid_date) if valid_date != 0xFFFFFFFF else '永久'
            status = '黑名单' if blacklist else '正常'
            
            cards_data.append({
                'uid': uid_str,
                'plate': plate,
                'balance': balance,
                'status': status,
                'valid_date': valid_date_str
            })
        
        self._cards_append(cards_data)
    
    def _handle_cards_ui(self, cards_data):
        # 卡片落库统一在 Tk 主线程完成，避免串口线程长时间持锁
        for card in cards_data:
            blacklist = 1 if card['status'] == '黑名单' else 0
            self.sync_card_to_db(card['uid'], card['plate'], card['balance'], blacklist)
        self.cards = cards_data
        self.update_cards_display()
        self.refresh_cards_list()
    
    def handle_card_data(self, data):
        # 最小长度：UID(4) + 有效标志(1) + 车牌(16) + 余额(4) + 黑名单(1) + 有效期(4) = 30
        if len(data) < 30:
            self._debug_log(f"卡片数据长度不足: {len(data)}")
            return
        uid = data[0:4]
        is_valid = data[4]
        card_plate = self.decode_plate(data[5:21])
        card_balance = struct.unpack('<I', data[21:25])[0]
        card_blacklist = data[25]
        valid_date = struct.unpack('<I', data[26:30])[0]
        
        uid_str = ''.join(f'{b:02X}' for b in uid)
        
        card_info = self.get_card_by_uid(uid_str)
        
        if card_info:
            plate = card_info[1] if card_info[1] else card_plate
            balance = card_info[2] if card_info[2] > 0 else card_balance
            blacklist = card_info[3]
            self.log(f"数据库对齐: UID={uid_str}, 使用数据库数据")
        else:
            plate = card_plate if is_valid else ''
            balance = card_balance if is_valid else 0
            blacklist = card_blacklist
            self.log(f"数据库无记录: UID={uid_str}, 使用卡片数据")
        
        self.uid_entry.delete(0, tk.END)
        self.uid_entry.insert(0, uid_str)
        self.plate_entry.delete(0, tk.END)
        self.plate_entry.insert(0, plate)
        self.balance_entry.delete(0, tk.END)
        self.balance_entry.insert(0, str(balance))
        self.blacklist_var.set(bool(blacklist))
        
        self.show_card_status(f"✓ 成功读取卡片 (有效={is_valid})", 'green')
        self.highlight_entries()
        
        self.log(f"读取卡片成功: UID={uid_str}, 有效={is_valid}, 车牌={plate}, 余额={balance}, 黑名单={blacklist}")
    
    def handle_time(self, data):
        if len(data) < 7:
            self._debug_log(f"时间数据包长度不足: {len(data)}")
            return
        year = struct.unpack('<H', data[0:2])[0]
        month = data[2]
        day = data[3]
        hour = data[4]
        minute = data[5]
        second = data[6]
        
        self.current_time = {
            'year': year,
            'month': month,
            'day': day,
            'hour': hour,
            'minute': minute,
            'second': second
        }
        
        self.use_local_time = False
        self.stm32_time_base = time.time() - (second + minute*60 + hour*3600)
        
        self.update_time_display()
        self.log(f"获取时间成功: {year}-{month:02d}-{day:02d} {hour:02d}:{minute:02d}:{second:02d}")
    
    def start_clock(self):
        self.update_clock()
    
    def update_clock(self):
        if self.use_local_time:
            now = time.localtime()
            self.current_time = {
                'year': now.tm_year,
                'month': now.tm_mon,
                'day': now.tm_mday,
                'hour': now.tm_hour,
                'minute': now.tm_min,
                'second': now.tm_sec
            }
        
        self.update_time_display()
        self.root.after(1000, self.update_clock)
    
    def start_status_refresh(self):
        self.refresh_status()
    
    def refresh_status(self):
        if self.ser and self.ser.is_open:
            self.request_status()
        self.root.after(2000, self.refresh_status)
    
    def update_time_display(self):
        t = self.current_time
        time_str = f"{t['year']}-{t['month']:02d}-{t['day']:02d} {t['hour']:02d}:{t['minute']:02d}:{t['second']:02d}"
        self.time_label.config(text=time_str)
    
    def get_time(self):
        self.use_local_time = False
        self.send_packet(0x21)
    
    def sync_time(self):
        now = time.localtime()
        year = now.tm_year
        month = now.tm_mon
        day = now.tm_mday
        hour = now.tm_hour
        minute = now.tm_min
        second = now.tm_sec
        
        data = bytearray()
        data.extend(struct.pack('<H', year))
        data.append(month)
        data.append(day)
        data.append(hour)
        data.append(minute)
        data.append(second)
        
        self.send_packet(0x06, data)
        self.log(f"同步时间到STM32: {year}-{month:02d}-{day:02d} {hour:02d}:{minute:02d}:{second:02d}")
    
    def sync_card_database(self):
        if not hasattr(self, 'db_cursor'):
            return
        
        with self.db_lock:
            self.db_cursor.execute("SELECT uid, plate, balance, blacklist FROM cards")
            cards = self.db_cursor.fetchall()
        
        if not cards:
            self.log("卡片数据库为空，无需同步")
            return
        
        self.log(f"同步卡片数据库: {len(cards)} 张卡片")
        
        for card in cards:
            uid_str, plate, balance, blacklist = card
            if not uid_str:
                continue
            
            uid = bytes.fromhex(uid_str)
            plate_bytes = plate.encode('gb2312')[:15] if plate else b''
            plate_bytes += b'\x00' * (15 - len(plate_bytes))
            
            data = bytearray(uid)
            data.extend(plate_bytes)
            data.extend(struct.pack('<I', balance or 0))
            data.append(1 if blacklist else 0)
            
            self.send_packet(0x23, data)
            time.sleep(0.05)
    
    def send_packet(self, cmd, data=b''):
        if not self.ser or not self.ser.is_open:
            return False
        
        packet = bytearray([0xAA, 0x55, cmd, len(data)])
        packet.extend(data)
        checksum = sum(packet[2:]) & 0xFF
        packet.append(checksum)
        
        try:
            with self.ser_lock:
                self.ser.write(packet)
            return True
        except Exception as e:
            self.log(f"发送失败: {e}")
            return False
    
    def request_status(self):
        self.send_packet(0x04)
    
    def request_records(self):
        self.send_packet(0x05)
    
    def request_cards(self):
        self.send_packet(0x0B)
    
    def clear_records(self):
        if messagebox.askyesno("确认", "确定清空所有记录?"):
            self.send_packet(0x0C)
    
    def add_card_dialog(self):
        dialog = tk.Toplevel(self.root)
        dialog.title("添加卡片")
        dialog.geometry("420x220")
        dialog.configure(bg=self.colors['bg'])
        dialog.resizable(False, False)

        shell = tk.Frame(dialog, bg=self.colors['bg'])
        shell.pack(fill=tk.BOTH, expand=True, padx=18, pady=18)

        card = ModernCard(shell, "添加卡片", self.colors, padding=10)
        card.pack(fill=tk.BOTH, expand=True)

        form = tk.Frame(card.content, bg=self.colors['card_bg'])
        form.pack(fill=tk.BOTH, expand=True)

        tk.Label(form, text="卡号(8位十六进制):", bg=self.colors['card_bg'], fg=self.colors['text'], font=('Microsoft YaHei', 11)).grid(row=0, column=0, padx=5, pady=8, sticky='e')
        uid_entry = ModernEntry(form, self.colors, width=20, font=('Microsoft YaHei', 11))
        uid_entry.grid(row=0, column=1, padx=5, pady=8)

        tk.Label(form, text="车牌号:", bg=self.colors['card_bg'], fg=self.colors['text'], font=('Microsoft YaHei', 11)).grid(row=1, column=0, padx=5, pady=8, sticky='e')
        plate_entry = ModernEntry(form, self.colors, width=20, font=('Microsoft YaHei', 11))
        plate_entry.grid(row=1, column=1, padx=5, pady=8)
        
        def ok():
            uid_str = uid_entry.get().strip()
            plate = plate_entry.get().strip()
            
            if len(uid_str) != 8:
                messagebox.showerror("错误", "卡号必须是8位十六进制")
                return
            
            try:
                uid = bytes.fromhex(uid_str)
            except:
                messagebox.showerror("错误", "卡号格式错误")
                return
            
            data = bytearray(uid)
            data.extend(plate.encode('gb2312'))
            data.extend(b'\x00' * (16 - len(plate)))
            
            self.send_packet(0x07, data)
            dialog.destroy()
        
        btn_row = tk.Frame(form, bg=self.colors['card_bg'])
        btn_row.grid(row=2, column=0, columnspan=2, pady=14)
        ModernButton(btn_row, text="确定", command=ok, palette=self.colors, variant='primary', width=100).pack(side=tk.LEFT, padx=6)
        ModernButton(btn_row, text="取消", command=dialog.destroy, palette=self.colors, variant='secondary', width=100).pack(side=tk.LEFT, padx=6)
    
    def delete_card(self):
        selected = self.cards_tree.selection()
        if not selected:
            messagebox.showwarning("警告", "请选择要删除的卡片")
            return
        
        item = self.cards_tree.item(selected[0])
        uid_str = item['values'][0]
        
        if messagebox.askyesno("确认", f"确定删除卡片 {uid_str}?"):
            uid = bytes.fromhex(uid_str)
            self.send_packet(0x08, uid)
    
    def set_card_blacklist_from_binding(self):
        uid_str = self.bind_uid_entry.get().strip()
        blacklist = self.bind_blacklist_var.get()
        
        if len(uid_str) != 8:
            messagebox.showerror("错误", "UID必须是8位十六进制")
            return
        
        try:
            with self.db_lock:
                self.db_cursor.execute(
                    "UPDATE cards SET blacklist=?, updated_at=CURRENT_TIMESTAMP WHERE uid=?",
                    (blacklist, uid_str)
                )
                self.db_conn.commit()
            self.refresh_cards_list()
            
            status_text = "加入黑名单" if blacklist else "解除黑名单"
            self.log(f"卡片{status_text}: UID={uid_str}")
            messagebox.showinfo("成功", f"卡片已{status_text}")
        except Exception as e:
            messagebox.showerror("错误", f"操作失败: {e}")
    
    def enter_config_mode(self):
        self.send_packet(0x10)
        self.config_mode_var.set("配置模式")
        self.enter_btn.config(state=tk.DISABLED)
        self.exit_btn.config(state=tk.NORMAL)
        self.show_card_status("请放置卡片到 RC522 感应区", 'orange')
        self.log("进入配置模式")
    
    def exit_config_mode(self):
        try:
            self.send_packet(0x11)
        except Exception as e:
            self.log(f"退出配置模式失败: {e}")
        
        self.config_mode_var.set("正常模式")
        self.enter_btn.config(state=tk.NORMAL)
        self.exit_btn.config(state=tk.DISABLED)
        self.show_card_status("已退出配置模式", 'gray')
        self.log("退出配置模式")
    
    def show_card_status(self, text, color='black'):
        self.card_status_label.config(text=text, foreground=color)
    
    def highlight_entries(self):
        try:
            self.uid_entry.config(background='#e0ffe0')
            self.plate_entry.config(background='#e0ffe0')
            self.balance_entry.config(background='#e0ffe0')
            
            self.root.after(1000, self.restore_entries_bg)
        except:
            pass
    
    def restore_entries_bg(self):
        try:
            self.uid_entry.config(background=self.original_bg)
            self.plate_entry.config(background=self.original_bg)
            self.balance_entry.config(background=self.original_bg)
        except:
            pass
    
    def read_card(self):
        self.show_card_status("正在读取卡片...", 'blue')
        self.send_packet(0x12)
    
    def write_card(self):
        uid_str = self.uid_entry.get().strip()
        plate = self.plate_entry.get().strip()
        balance = int(self.balance_entry.get() or '0')
        blacklist = 1 if self.blacklist_var.get() else 0
        
        if len(uid_str) != 8:
            messagebox.showerror("错误", "UID必须是8位十六进制")
            return
        
        self.show_card_status("正在写入卡片...", 'blue')
        
        self.add_card(uid_str, plate, balance)
        
        with self.db_lock:
            self.db_cursor.execute(
                "UPDATE cards SET blacklist=? WHERE uid=?",
                (blacklist, uid_str)
            )
            self.db_conn.commit()
        
        uid = bytes.fromhex(uid_str)
        plate_bytes = plate.encode('gb2312')[:15]
        plate_bytes += b'\x00' * (15 - len(plate_bytes))
        
        data = bytearray(uid)
        data.append(1)
        data.extend(plate_bytes)
        data.extend(struct.pack('<I', balance))
        data.append(blacklist)
        data.extend(struct.pack('<I', 0xFFFFFFFF))
        
        self.send_packet(0x13, data)
        
        self.refresh_cards_list()
        self.log(f"卡片绑定完成: UID={uid_str}, 车牌={plate}, 余额={balance}, 黑名单={blacklist}")
    
    def set_plate(self):
        uid_str = self.uid_entry.get().strip()
        plate = self.plate_entry.get().strip()
        self.show_card_status("正在设置车牌...", 'blue')
        
        if uid_str:
            with self.db_lock:
                self.db_cursor.execute(
                    "UPDATE cards SET plate=?, updated_at=CURRENT_TIMESTAMP WHERE uid=?",
                    (plate, uid_str)
                )
                self.db_conn.commit()
            self.refresh_cards_list()
            self.log(f"数据库更新车牌: UID={uid_str}, 车牌={plate}")
        
        plate_bytes = plate.encode('gb2312')[:15]
        plate_bytes += b'\x00' * (15 - len(plate_bytes))
        
        data = bytearray(4)
        data.extend(plate_bytes)
        
        self.send_packet(0x14, data)
    
    def set_balance(self):
        uid_str = self.uid_entry.get().strip()
        balance = int(self.balance_entry.get() or '0')
        self.show_card_status("正在设置余额...", 'blue')
        
        if uid_str:
            with self.db_lock:
                self.db_cursor.execute(
                    "UPDATE cards SET balance=?, updated_at=CURRENT_TIMESTAMP WHERE uid=?",
                    (balance, uid_str)
                )
                self.db_conn.commit()
            self.refresh_cards_list()
            self.log(f"数据库更新余额: UID={uid_str}, 余额={balance}")
        
        data = bytearray(4)
        data.extend(struct.pack('<I', balance))
        
        self.send_packet(0x15, data)
    
    def set_blacklist(self):
        uid_str = self.uid_entry.get().strip()
        blacklist = 1 if self.blacklist_var.get() else 0
        status_text = "正在设置黑名单..." if blacklist else "正在取消黑名单..."
        self.show_card_status(status_text, 'blue')
        
        if uid_str:
            with self.db_lock:
                self.db_cursor.execute(
                    "UPDATE cards SET blacklist=?, updated_at=CURRENT_TIMESTAMP WHERE uid=?",
                    (blacklist, uid_str)
                )
                self.db_conn.commit()
            self.refresh_cards_list()
            self.log(f"数据库更新黑名单: UID={uid_str}, 黑名单={blacklist}")
        
        data = bytearray(4)
        data.append(blacklist)
        
        self.send_packet(0x16, data)
    
    def update_space_display(self):
        for i in range(3):
            occupied = self.parking_status['spaces'][i]
            status_indicator, status_text = self.space_frames[i]
            
            if occupied:
                status_indicator.config(text="●", foreground='#F44336')
                status_text.config(text=f"车位 {i+1}: 占用", foreground='#F44336')
            else:
                status_indicator.config(text="●", foreground='#4CAF50')
                status_text.config(text=f"车位 {i+1}: 空闲", foreground='#4CAF50')
        
        total = self.parking_status['total_spaces']
        free = total - self.vehicle_count
        
        self.free_display.config(text=f"{free}")
        
        if free == total:
            self.free_display.config(foreground='#4CAF50')
        elif free > total / 2:
            self.free_display.config(foreground='#FF9800')
        else:
            self.free_display.config(foreground='#F44336')
        
        if hasattr(self, 'space_stats_label'):
            self.space_stats_label.config(text=f"空闲: {free}/{total}")
    
    def update_records_display(self):
        for item in self.records_tree.get_children():
            self.records_tree.delete(item)

        if not self.records:
            self.records_tree.insert('', tk.END, values=('暂无记录', '-', '-', '-', '-'), tags=('odd',))
            return
        
        for index, rec in enumerate(self.records[:50]):
            tag = 'even' if index % 2 else 'odd'
            self.records_tree.insert('', tk.END, values=(
                rec['plate'], rec['uid'], rec['in_time'], rec['out_time'], rec['fee']
            ), tags=(tag,))
    
    def update_cards_display(self):
        for item in self.cards_tree.get_children():
            self.cards_tree.delete(item)

        if not self.cards:
            self.cards_tree.insert('', tk.END, values=('暂无卡片', '-', '-', '-'), tags=('odd',))
            return
        
        for index, card in enumerate(self.cards):
            tag = 'even' if index % 2 else 'odd'
            self.cards_tree.insert('', tk.END, values=(
                card['uid'], card['plate'], card['balance'], card['status']
            ), tags=(tag,))
    
    def log(self, msg):
        self.root.after(0, self._log_ui, msg)
    
    def _log_ui(self, msg):
        self.log_text.insert(tk.END, f"{time.strftime('%H:%M:%S')} - {msg}\n")
        self.log_text.see(tk.END)

    def update_connection_status(self, connected, detail="未连接"):
        status_text = detail if connected else "未连接"
        status_color = self.colors['success'] if connected else self.colors['danger']
        self.status_bar.config(text=status_text)
        self.topbar_status_label.config(text=status_text)
        self.status_indicator.delete('all')
        self.status_indicator.create_oval(2, 2, 14, 14, fill=status_color, outline='')
    
    def timestamp_to_str(self, ts):
        """把固件时间戳解码为 MM/DD HH:MM:SS。

        时间语义与固件一致：epoch = 2000-01-01 00:00:00 本地时间的 32 位秒数；
        ts == 0 表示无效时间，返回 '-'。
        """
        if not ts:
            return '-'
        try:
            dt = datetime.datetime(2000, 1, 1) + datetime.timedelta(seconds=int(ts))
            return dt.strftime('%m/%d %H:%M:%S')
        except (OverflowError, ValueError, TypeError) as e:
            self._debug_log(f"时间戳解码失败: ts={ts}, {e}")
            return '-'
    
    def connect_serial(self, port, baud=115200):
        try:
            self.ser = serial.Serial(port, baud, timeout=1)
            self.update_connection_status(True, f"已连接: {port} @ {baud}")
            self.log(f"串口连接成功: {port}")
            time.sleep(0.5)
            
            try:
                self.sync_time()
                time.sleep(0.2)
            except Exception as e:
                self.log(f"时间同步失败: {e}")
            
            try:
                self.request_status()
            except Exception as e:
                self.log(f"状态请求失败: {e}")
            
            try:
                self.sync_card_database()
            except Exception as e:
                self.log(f"卡片数据库同步失败: {e}")
                
            return True
        except Exception as e:
            messagebox.showerror("错误", f"连接失败: {e}")
            return False
    
    def close_serial(self):
        if self.ser:
            self.ser.close()
            self.ser = None
        self.update_connection_status(False)
        self.log("串口已关闭")

def get_serial_ports():
    ports = serial.tools.list_ports.comports()
    port_list = []
    for port in ports:
        desc = f"{port.device}"
        if port.description and port.description != "n/a":
            desc += f" - {port.description}"
        port_list.append((port.device, desc))
    return port_list

class ConnectDialog:
    def __init__(self, parent, callback, palette=None):
        self.parent = parent
        self.callback = callback
        self.palette = palette or {
            'bg': '#eef2ff',
            'card_bg': '#ffffff',
            'text': '#16213e',
            'muted': '#6b7280',
            'primary': '#4f46e5',
            'primary_hover': '#4338ca',
            'secondary_btn': '#e8ecff',
            'secondary_hover': '#dbe4ff',
            'danger': '#ef4444',
            'danger_hover': '#dc2626',
            'success': '#10b981',
            'success_hover': '#059669',
            'sidebar_btn': '#0f172a',
            'sidebar_hover': '#1e293b',
            'sidebar_active': '#334155',
            'sidebar_text': '#cbd5e1',
            'entry_bg': '#f8fbff',
            'entry_border': '#d0daf0',
            'entry_focus': '#4f46e5',
            'disabled': '#c7d2fe',
            'disabled_text': '#eff6ff',
            'shadow': '#dfe6f8',
            'card_border': '#d9e1f2',
        }
        
        self.dialog = tk.Toplevel(parent)
        self.dialog.title("连接串口")
        self.dialog.geometry("560x300")
        self.dialog.configure(bg=self.palette['bg'])
        self.dialog.resizable(False, False)

        shell = tk.Frame(self.dialog, bg=self.palette['bg'])
        shell.pack(fill=tk.BOTH, expand=True, padx=18, pady=18)

        card = ModernCard(shell, "串口连接", self.palette, padding=10)
        card.pack(fill=tk.BOTH, expand=True)

        form = tk.Frame(card.content, bg=self.palette['card_bg'])
        form.pack(fill=tk.BOTH, expand=True)
        form.columnconfigure(1, weight=1)

        tk.Label(form, text="串口端口:", bg=self.palette['card_bg'], fg=self.palette['text'], font=('Microsoft YaHei', 11)).grid(row=0, column=0, padx=5, pady=10, sticky='e')
        self.port_devices = []
        self.port_combo = ttk.Combobox(form, width=38, state="readonly")
        self.port_combo.grid(row=0, column=1, padx=5, pady=10, sticky='we')
        self.refresh_ports()
        
        ModernButton(form, text="刷新", command=self.refresh_ports, palette=self.palette, variant='secondary', width=92).grid(row=0, column=2, padx=5, pady=10)
        
        tk.Label(form, text="波特率:", bg=self.palette['card_bg'], fg=self.palette['text'], font=('Microsoft YaHei', 11)).grid(row=1, column=0, padx=5, pady=5, sticky='e')
        self.baud_var = tk.StringVar(value="115200")
        ttk.Combobox(form, textvariable=self.baud_var, values=['9600', '19200', '38400', '57600', '115200'], width=38, state="readonly").grid(row=1, column=1, padx=5, pady=5, sticky='we')
        
        btn_frame = tk.Frame(form, bg=self.palette['card_bg'])
        btn_frame.grid(row=2, column=0, columnspan=3, pady=16)
        
        ModernButton(btn_frame, text="连接", command=self.connect, palette=self.palette, variant='primary', width=110).pack(side=tk.LEFT, padx=6)
        ModernButton(btn_frame, text="取消", command=self.dialog.destroy, palette=self.palette, variant='secondary', width=110).pack(side=tk.LEFT, padx=6)
    
    def refresh_ports(self):
        ports = get_serial_ports()
        self.port_devices = [p[0] for p in ports]
        self.port_combo['values'] = [p[1] for p in ports]
        if ports:
            self.port_combo.current(0)
    
    def connect(self):
        selected_index = self.port_combo.current()
        if selected_index < 0:
            messagebox.showerror("错误", "请选择一个串口")
            return
        
        port = self.port_devices[selected_index]
        baud = int(self.baud_var.get())
        
        if self.callback(port, baud):
            self.dialog.destroy()

if __name__ == "__main__":
    root = tk.Tk()
    app = ParkingClient(root)
    
    root.mainloop()

