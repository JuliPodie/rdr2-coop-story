using System.ComponentModel;
using System.Drawing.Drawing2D;

namespace CoopStory.Launcher;

internal static class LauncherTheme
{
    public static readonly Color Background = Color.FromArgb(12, 6, 6);
    public static readonly Color BackgroundLift = Color.FromArgb(19, 9, 9);
    public static readonly Color Bar = Color.FromArgb(65, 8, 8);
    public static readonly Color Surface = Color.FromArgb(25, 14, 14);
    public static readonly Color SurfaceRaised = Color.FromArgb(35, 20, 20);
    public static readonly Color SurfaceHover = Color.FromArgb(49, 25, 25);
    public static readonly Color Border = Color.FromArgb(76, 43, 43);
    public static readonly Color BorderStrong = Color.FromArgb(111, 47, 47);
    public static readonly Color Red = Color.FromArgb(139, 21, 21);
    public static readonly Color RedBright = Color.FromArgb(190, 35, 31);
    public static readonly Color Text = Color.FromArgb(246, 239, 225);
    public static readonly Color TextMuted = Color.FromArgb(174, 155, 144);
    public static readonly Color TextDim = Color.FromArgb(124, 103, 98);
    public static readonly Color Success = Color.FromArgb(115, 191, 139);
    public static readonly Color Warning = Color.FromArgb(232, 176, 92);
    public static readonly Color Failure = Color.FromArgb(235, 101, 91);
    public static readonly Color Host = Color.FromArgb(205, 72, 55);
    public static readonly Color Guest = Color.FromArgb(73, 145, 210);
    public static readonly Color Steam = Color.FromArgb(33, 49, 75);
    public static readonly Color Rockstar = Color.FromArgb(240, 169, 44);

    public static GraphicsPath RoundedRectangle(Rectangle bounds, int radius)
    {
        var diameter = Math.Min(radius * 2, Math.Min(bounds.Width, bounds.Height));
        var path = new GraphicsPath();
        if (diameter <= 0)
        {
            path.AddRectangle(bounds);
            return path;
        }

        var arc = new Rectangle(bounds.X, bounds.Y, diameter, diameter);
        path.AddArc(arc, 180, 90);
        arc.X = bounds.Right - diameter;
        path.AddArc(arc, 270, 90);
        arc.Y = bounds.Bottom - diameter;
        path.AddArc(arc, 0, 90);
        arc.X = bounds.X;
        path.AddArc(arc, 90, 90);
        path.CloseFigure();
        return path;
    }

    public static Color Blend(Color from, Color to, float amount)
    {
        var value = Math.Clamp(amount, 0f, 1f);
        return Color.FromArgb(
            (int)(from.A + ((to.A - from.A) * value)),
            (int)(from.R + ((to.R - from.R) * value)),
            (int)(from.G + ((to.G - from.G) * value)),
            (int)(from.B + ((to.B - from.B) * value)));
    }
}

internal sealed class RdrBufferedPanel : Panel
{
    public RdrBufferedPanel()
    {
        SetStyle(
            ControlStyles.AllPaintingInWmPaint |
            ControlStyles.OptimizedDoubleBuffer |
            ControlStyles.ResizeRedraw |
            ControlStyles.UserPaint,
            true);
    }

    protected override CreateParams CreateParams
    {
        get
        {
            const int WsExComposited = 0x02000000;
            var parameters = base.CreateParams;
            parameters.ExStyle |= WsExComposited;
            return parameters;
        }
    }
}

internal sealed class RdrLobbyPanel : Control
{
    private LauncherLobbySnapshot _snapshot = LauncherLobbySnapshot.Empty;

    public RdrLobbyPanel()
    {
        SetStyle(
            ControlStyles.AllPaintingInWmPaint |
            ControlStyles.OptimizedDoubleBuffer |
            ControlStyles.ResizeRedraw |
            ControlStyles.UserPaint,
            true);
        BackColor = LauncherTheme.BackgroundLift;
        Height = 122;
        AccessibleName = "Session lobby";
    }

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public LauncherLobbySnapshot Snapshot
    {
        get => _snapshot;
        set
        {
            _snapshot = value ?? LauncherLobbySnapshot.Empty;
            AccessibleDescription = BuildAccessibleDescription(_snapshot);
            Invalidate();
        }
    }

    protected override void OnPaint(PaintEventArgs eventArgs)
    {
        base.OnPaint(eventArgs);
        var graphics = eventArgs.Graphics;
        graphics.SmoothingMode = SmoothingMode.AntiAlias;
        graphics.Clear(LauncherTheme.BackgroundLift);

        using var headingFont = new Font(Font, FontStyle.Bold);
        TextRenderer.DrawText(
            graphics,
            "SESSION LOBBY",
            headingFont,
            new Rectangle(12, 6, Width - 24, 19),
            LauncherTheme.TextMuted,
            TextFormatFlags.Left | TextFormatFlags.VerticalCenter);

        var gap = 8;
        var cardWidth = Math.Max(90, (Width - 24 - gap) / 2);
        DrawParticipant(
            graphics,
            new Rectangle(12, 30, cardWidth, 78),
            _snapshot.LocalNickname,
            _snapshot.LocalRole,
            local: true,
            connected: _snapshot.SidecarRunning,
            pingMilliseconds: null);
        DrawParticipant(
            graphics,
            new Rectangle(12 + cardWidth + gap, 30, cardWidth, 78),
            string.IsNullOrWhiteSpace(_snapshot.RemoteNickname)
                ? (_snapshot.PeerConnected
                    ? "Connected player"
                    : "Waiting...")
                : _snapshot.RemoteNickname,
            _snapshot.RemoteRole,
            local: false,
            connected: _snapshot.PeerConnected,
            pingMilliseconds: _snapshot.PingMilliseconds);
    }

    private void DrawParticipant(
        Graphics graphics,
        Rectangle bounds,
        string nickname,
        LauncherRole role,
        bool local,
        bool connected,
        long? pingMilliseconds)
    {
        var accent = role == LauncherRole.Host
            ? LauncherTheme.Host
            : LauncherTheme.Guest;
        using var path = LauncherTheme.RoundedRectangle(bounds, 12);
        using var fill = new SolidBrush(
            LauncherTheme.Blend(LauncherTheme.SurfaceRaised, accent, 0.10f));
        using var outline = new Pen(
            connected ? accent : LauncherTheme.Border,
            connected ? 1.6f : 1f);
        graphics.FillPath(fill, path);
        graphics.DrawPath(outline, path);
        using var accentBrush = new SolidBrush(accent);
        graphics.FillEllipse(
            accentBrush,
            bounds.Left + 10,
            bounds.Top + 12,
            9,
            9);

        var roleText = role == LauncherRole.Host ? "HOST" : "GUEST";
        using var roleFont = new Font(Font.FontFamily, 7.6f, FontStyle.Bold);
        using var nicknameFont = new Font(Font, FontStyle.Bold);
        TextRenderer.DrawText(
            graphics,
            roleText,
            roleFont,
            new Rectangle(bounds.Left + 25, bounds.Top + 6, 62, 22),
            accent,
            TextFormatFlags.Left | TextFormatFlags.VerticalCenter);
        TextRenderer.DrawText(
            graphics,
            nickname,
            nicknameFont,
            new Rectangle(bounds.Left + 10, bounds.Top + 30, bounds.Width - 20, 23),
            LauncherTheme.Text,
            TextFormatFlags.Left | TextFormatFlags.VerticalCenter |
            TextFormatFlags.EndEllipsis);

        var state = local
            ? (_snapshot.GameBridgeConnected
                ? "RDR2 connected"
                : connected ? "Sidecar ready" : "Configuring")
            : connected
                ? pingMilliseconds.HasValue
                    ? $"ONLINE  •  {pingMilliseconds.Value} ms"
                    : "ONLINE  •  ping —"
                : "OFFLINE";
        TextRenderer.DrawText(
            graphics,
            state,
            roleFont,
            new Rectangle(bounds.Left + 10, bounds.Top + 54, bounds.Width - 20, 18),
            connected ? LauncherTheme.Success : LauncherTheme.TextDim,
            TextFormatFlags.Left | TextFormatFlags.VerticalCenter |
            TextFormatFlags.EndEllipsis);
    }

    private static string BuildAccessibleDescription(
        LauncherLobbySnapshot snapshot) =>
        $"{snapshot.LocalNickname}, {snapshot.LocalRole}; " +
        $"{(string.IsNullOrWhiteSpace(snapshot.RemoteNickname) ? "no second player" : snapshot.RemoteNickname)}, " +
        $"{snapshot.RemoteRole}; ping " +
        $"{(snapshot.PingMilliseconds.HasValue ? snapshot.PingMilliseconds.Value + " ms" : "unavailable")}.";
}

internal sealed class RdrSurface : Panel
{
    public RdrSurface()
    {
        SetStyle(
            ControlStyles.AllPaintingInWmPaint |
            ControlStyles.OptimizedDoubleBuffer |
            ControlStyles.ResizeRedraw |
            ControlStyles.SupportsTransparentBackColor |
            ControlStyles.UserPaint,
            true);
        BackColor = Color.Transparent;
    }

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public int CornerRadius { get; set; } = 18;

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public Color FillColor { get; set; } = LauncherTheme.Surface;

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public Color OutlineColor { get; set; } = LauncherTheme.Border;

    protected override void OnPaintBackground(PaintEventArgs eventArgs)
    {
        eventArgs.Graphics.Clear(Parent?.BackColor ?? LauncherTheme.Background);
        eventArgs.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
        using var path = LauncherTheme.RoundedRectangle(
            new Rectangle(0, 0, Width - 1, Height - 1),
            CornerRadius);
        using var fill = new SolidBrush(FillColor);
        using var outline = new Pen(OutlineColor);
        eventArgs.Graphics.FillPath(fill, path);
        eventArgs.Graphics.DrawPath(outline, path);
    }
}

internal sealed class RdrBackdrop : Panel
{
    public RdrBackdrop()
    {
        SetStyle(
            ControlStyles.AllPaintingInWmPaint |
            ControlStyles.OptimizedDoubleBuffer |
            ControlStyles.ResizeRedraw |
            ControlStyles.UserPaint,
            true);
        BackColor = LauncherTheme.Background;
    }

    protected override void OnPaintBackground(PaintEventArgs eventArgs)
    {
        var graphics = eventArgs.Graphics;
        graphics.Clear(LauncherTheme.Background);
        graphics.SmoothingMode = SmoothingMode.AntiAlias;
        using var leftBrush = new LinearGradientBrush(
            ClientRectangle,
            Color.FromArgb(120, LauncherTheme.Bar),
            Color.FromArgb(0, LauncherTheme.Bar),
            LinearGradientMode.Horizontal);
        using var rightBrush = new SolidBrush(Color.FromArgb(18, LauncherTheme.Red));
        graphics.FillPolygon(
            leftBrush,
        [
            new Point(0, 0),
            new Point(Math.Max(220, Width / 3), 0),
            new Point(Math.Max(90, Width / 8), Height),
            new Point(0, Height)
        ]);
        graphics.FillPolygon(
            rightBrush,
        [
            new Point(Width, Math.Max(0, Height / 5)),
            new Point(Width, Height),
            new Point(Math.Max(0, Width - 280), Height)
        ]);
    }
}

internal sealed class RdrModeCard : Control
{
    private readonly System.Windows.Forms.Timer _animationTimer;
    private float _hoverAmount;
    private float _hoverTarget;
    private bool _selected;

    public RdrModeCard()
    {
        SetStyle(
            ControlStyles.AllPaintingInWmPaint |
            ControlStyles.OptimizedDoubleBuffer |
            ControlStyles.ResizeRedraw |
            ControlStyles.Selectable |
            ControlStyles.SupportsTransparentBackColor |
            ControlStyles.UserPaint,
            true);
        BackColor = Color.Transparent;
        Cursor = Cursors.Hand;
        TabStop = true;
        Height = 84;
        _animationTimer = new System.Windows.Forms.Timer { Interval = 16 };
        _animationTimer.Tick += (_, _) => Animate();
    }

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public string TitleText { get; set; } = string.Empty;

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public string DescriptionText { get; set; } = string.Empty;

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public string BadgeText { get; set; } = string.Empty;

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public Color AccentColor { get; set; } = LauncherTheme.Red;

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public bool Selected
    {
        get => _selected;
        set
        {
            if (_selected == value)
            {
                return;
            }

            _selected = value;
            Invalidate();
        }
    }

    protected override void OnMouseEnter(EventArgs eventArgs)
    {
        base.OnMouseEnter(eventArgs);
        _hoverTarget = 1f;
        _animationTimer.Start();
    }

    protected override void OnMouseLeave(EventArgs eventArgs)
    {
        base.OnMouseLeave(eventArgs);
        _hoverTarget = 0f;
        _animationTimer.Start();
    }

    protected override void OnGotFocus(EventArgs eventArgs)
    {
        base.OnGotFocus(eventArgs);
        Invalidate();
    }

    protected override void OnLostFocus(EventArgs eventArgs)
    {
        base.OnLostFocus(eventArgs);
        Invalidate();
    }

    protected override void OnKeyDown(KeyEventArgs eventArgs)
    {
        base.OnKeyDown(eventArgs);
        if (eventArgs.KeyCode is Keys.Enter or Keys.Space)
        {
            OnClick(EventArgs.Empty);
            eventArgs.Handled = true;
        }
    }

    protected override void OnPaint(PaintEventArgs eventArgs)
    {
        base.OnPaint(eventArgs);
        var graphics = eventArgs.Graphics;
        graphics.SmoothingMode = SmoothingMode.AntiAlias;
        graphics.TextRenderingHint = System.Drawing.Text.TextRenderingHint.ClearTypeGridFit;
        var bounds = new Rectangle(0, 0, Width - 1, Height - 1);
        using var path = LauncherTheme.RoundedRectangle(bounds, 14);
        var selectedFill = LauncherTheme.Blend(
            LauncherTheme.SurfaceRaised,
            AccentColor,
            0.18f);
        var fillColor = Selected
            ? selectedFill
            : LauncherTheme.Blend(
                LauncherTheme.SurfaceRaised,
                LauncherTheme.SurfaceHover,
                _hoverAmount);
        var outlineColor = Selected
            ? AccentColor
            : LauncherTheme.Blend(
                LauncherTheme.Border,
                LauncherTheme.BorderStrong,
                _hoverAmount);
        using var fill = new SolidBrush(fillColor);
        using var outline = new Pen(outlineColor, Selected ? 2f : 1f);
        graphics.FillPath(fill, path);
        graphics.DrawPath(outline, path);

        if (Selected)
        {
            using var accent = new SolidBrush(AccentColor);
            graphics.FillRoundedRectangle(
                accent,
                new Rectangle(0, 16, 4, Height - 32),
                new Size(4, 4));
        }

        var titleBounds = new Rectangle(18, 15, Width - 36, 24);
        using var titleFont = new Font(Font, FontStyle.Bold);
        TextRenderer.DrawText(
            graphics,
            TitleText,
            titleFont,
            titleBounds,
            LauncherTheme.Text,
            TextFormatFlags.Left | TextFormatFlags.VerticalCenter |
            TextFormatFlags.EndEllipsis);
        var descriptionBounds = new Rectangle(18, 43, Width - 36, 27);
        TextRenderer.DrawText(
            graphics,
            DescriptionText,
            Font,
            descriptionBounds,
            LauncherTheme.TextMuted,
            TextFormatFlags.Left | TextFormatFlags.Top |
            TextFormatFlags.EndEllipsis);

        if (!string.IsNullOrWhiteSpace(BadgeText))
        {
            using var badgeFont = new Font(
                Font.FontFamily,
                Math.Max(7.5f, Font.Size - 2f),
                FontStyle.Bold);
            var badgeSize = TextRenderer.MeasureText(BadgeText, badgeFont);
            var badgeBounds = new Rectangle(
                Width - badgeSize.Width - 22,
                14,
                badgeSize.Width + 10,
                22);
            using var badgePath = LauncherTheme.RoundedRectangle(badgeBounds, 8);
            using var badgeFill = new SolidBrush(
                Color.FromArgb(90, AccentColor));
            graphics.FillPath(badgeFill, badgePath);
            TextRenderer.DrawText(
                graphics,
                BadgeText,
                badgeFont,
                badgeBounds,
                LauncherTheme.Text,
                TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
        }

        if (Focused)
        {
            ControlPaint.DrawFocusRectangle(graphics, Rectangle.Inflate(bounds, -5, -5));
        }
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            _animationTimer.Dispose();
        }

        base.Dispose(disposing);
    }

    private void Animate()
    {
        _hoverAmount += (_hoverTarget - _hoverAmount) * 0.28f;
        if (Math.Abs(_hoverTarget - _hoverAmount) < 0.02f)
        {
            _hoverAmount = _hoverTarget;
            _animationTimer.Stop();
        }

        Invalidate();
    }
}

internal sealed class RdrStartOrb : Control
{
    private readonly System.Windows.Forms.Timer _timer;
    private float _phase;
    private bool _busy;

    public RdrStartOrb()
    {
        SetStyle(
            ControlStyles.AllPaintingInWmPaint |
            ControlStyles.OptimizedDoubleBuffer |
            ControlStyles.ResizeRedraw |
            ControlStyles.Selectable |
            ControlStyles.SupportsTransparentBackColor |
            ControlStyles.UserPaint,
            true);
        BackColor = Color.Transparent;
        Cursor = Cursors.Hand;
        TabStop = true;
        Size = new Size(190, 190);
        // A full transparent repaint at ~42 FPS caused visible black child
        // placeholders while changing pages on slower GPUs. Twelve FPS keeps
        // the subtle pulse without starving WinForms layout and paint work.
        _timer = new System.Windows.Forms.Timer { Interval = 84 };
        _timer.Tick += (_, _) =>
        {
            _phase = (_phase + 0.035f) % 1f;
            Invalidate();
        };
    }

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public string MainText { get; set; } = "START";

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public string DetailText { get; set; } = "SELECT A MODE";

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public Color AccentColor { get; set; } = LauncherTheme.Red;

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public Font? DisplayFont { get; set; }

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public bool Busy
    {
        get => _busy;
        set
        {
            _busy = value;
            UpdateAnimationState();
            Invalidate();
        }
    }

    protected override void OnEnabledChanged(EventArgs eventArgs)
    {
        base.OnEnabledChanged(eventArgs);
        Cursor = Enabled ? Cursors.Hand : Cursors.Default;
        UpdateAnimationState();
        Invalidate();
    }

    protected override void OnVisibleChanged(EventArgs eventArgs)
    {
        base.OnVisibleChanged(eventArgs);
        UpdateAnimationState();
    }

    protected override void OnMouseEnter(EventArgs eventArgs)
    {
        base.OnMouseEnter(eventArgs);
        Invalidate();
    }

    protected override void OnMouseLeave(EventArgs eventArgs)
    {
        base.OnMouseLeave(eventArgs);
        Invalidate();
    }

    protected override void OnKeyDown(KeyEventArgs eventArgs)
    {
        base.OnKeyDown(eventArgs);
        if (Enabled && eventArgs.KeyCode is Keys.Enter or Keys.Space)
        {
            OnClick(EventArgs.Empty);
            eventArgs.Handled = true;
        }
    }

    protected override void OnPaint(PaintEventArgs eventArgs)
    {
        base.OnPaint(eventArgs);
        var graphics = eventArgs.Graphics;
        graphics.SmoothingMode = SmoothingMode.AntiAlias;
        graphics.TextRenderingHint = System.Drawing.Text.TextRenderingHint.AntiAliasGridFit;
        var diameter = Math.Min(Width, Height) - 18;
        var circle = new Rectangle(
            (Width - diameter) / 2,
            (Height - diameter) / 2,
            diameter,
            diameter);
        var pulse = (float)((Math.Sin(_phase * Math.PI * 2) + 1d) / 2d);
        var activeAccent = Enabled ? AccentColor : LauncherTheme.Border;
        var glowAlpha = Enabled ? (int)(34 + (pulse * 30)) : 12;

        using (var glow = new Pen(Color.FromArgb(glowAlpha, activeAccent), 8f))
        {
            graphics.DrawEllipse(glow, Rectangle.Inflate(circle, 3, 3));
        }

        using var gradient = new PathGradientBrush(new[]
        {
            new Point(circle.Left, circle.Top),
            new Point(circle.Right, circle.Top),
            new Point(circle.Right, circle.Bottom),
            new Point(circle.Left, circle.Bottom)
        })
        {
            CenterColor = Enabled
                ? LauncherTheme.Blend(activeAccent, Color.White, 0.08f)
                : LauncherTheme.SurfaceRaised,
            SurroundColors = Enumerable.Repeat(
                Enabled
                    ? LauncherTheme.Blend(activeAccent, Color.Black, 0.25f)
                    : LauncherTheme.Surface,
                4).ToArray()
        };
        graphics.FillEllipse(gradient, circle);
        using var outline = new Pen(
            Enabled ? LauncherTheme.Blend(activeAccent, Color.White, 0.3f) : LauncherTheme.Border,
            MouseRectangle.Contains(PointToClient(MousePosition)) && Enabled ? 3f : 2f);
        graphics.DrawEllipse(outline, circle);

        var title = Busy ? "PLEASE WAIT" : MainText;
        using var titleFont = DisplayFont is not null
            ? new Font(DisplayFont.FontFamily, DisplayFont.Size, FontStyle.Regular)
            : new Font("Georgia", 20f, FontStyle.Bold);
        var titleBounds = new Rectangle(circle.Left + 16, circle.Top + 58, circle.Width - 32, 44);
        var textColor = Enabled
            ? AccentColor == LauncherTheme.Rockstar
                ? Color.FromArgb(35, 24, 12)
                : Color.White
            : LauncherTheme.TextDim;
        TextRenderer.DrawText(
            graphics,
            title,
            titleFont,
            titleBounds,
            textColor,
            TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter |
            TextFormatFlags.EndEllipsis);
        var detailBounds = new Rectangle(circle.Left + 20, circle.Top + 105, circle.Width - 40, 34);
        using var detailFont = new Font(
            Font.FontFamily,
            Math.Max(8f, Font.Size - 1f),
            FontStyle.Bold);
        TextRenderer.DrawText(
            graphics,
            Busy ? "PREPARING" : DetailText,
            detailFont,
            detailBounds,
            Enabled ? textColor : LauncherTheme.TextDim,
            TextFormatFlags.HorizontalCenter | TextFormatFlags.Top |
            TextFormatFlags.EndEllipsis);

        if (Focused)
        {
            ControlPaint.DrawFocusRectangle(graphics, Rectangle.Inflate(circle, -8, -8));
        }
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            _timer.Dispose();
        }

        base.Dispose(disposing);
    }

    private Rectangle MouseRectangle => ClientRectangle;

    private void UpdateAnimationState()
    {
        if (Visible && (Enabled || Busy))
        {
            _timer.Start();
        }
        else
        {
            _timer.Stop();
        }
    }
}

internal enum RdrIcon
{
    None,
    Home,
    Settings,
    Folder,
    Search,
    Download,
    Trash,
    Shield,
    Copy,
    Eye,
    Link,
    Refresh,
    Stop,
    Log,
    Arrow
}

internal sealed class RdrActionButton : Control
{
    private bool _hovered;
    private bool _pressed;

    public RdrActionButton()
    {
        SetStyle(
            ControlStyles.AllPaintingInWmPaint |
            ControlStyles.OptimizedDoubleBuffer |
            ControlStyles.ResizeRedraw |
            ControlStyles.Selectable |
            ControlStyles.SupportsTransparentBackColor |
            ControlStyles.UserPaint,
            true);
        BackColor = Color.Transparent;
        Cursor = Cursors.Hand;
        TabStop = true;
        Height = 38;
    }

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public RdrIcon Icon { get; set; }

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public bool Accent { get; set; }

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public bool Danger { get; set; }

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public bool Compact { get; set; }

    protected override void OnEnabledChanged(EventArgs eventArgs)
    {
        base.OnEnabledChanged(eventArgs);
        Cursor = Enabled ? Cursors.Hand : Cursors.Default;
        Invalidate();
    }

    protected override void OnMouseEnter(EventArgs eventArgs)
    {
        base.OnMouseEnter(eventArgs);
        _hovered = true;
        Invalidate();
    }

    protected override void OnMouseLeave(EventArgs eventArgs)
    {
        base.OnMouseLeave(eventArgs);
        _hovered = false;
        _pressed = false;
        Invalidate();
    }

    protected override void OnMouseDown(MouseEventArgs eventArgs)
    {
        base.OnMouseDown(eventArgs);
        if (eventArgs.Button == MouseButtons.Left)
        {
            _pressed = true;
            Invalidate();
        }
    }

    protected override void OnMouseUp(MouseEventArgs eventArgs)
    {
        base.OnMouseUp(eventArgs);
        _pressed = false;
        Invalidate();
    }

    protected override void OnKeyDown(KeyEventArgs eventArgs)
    {
        base.OnKeyDown(eventArgs);
        if (Enabled && eventArgs.KeyCode is Keys.Enter or Keys.Space)
        {
            OnClick(EventArgs.Empty);
            eventArgs.Handled = true;
        }
    }

    protected override void OnPaint(PaintEventArgs eventArgs)
    {
        base.OnPaint(eventArgs);
        var graphics = eventArgs.Graphics;
        graphics.SmoothingMode = SmoothingMode.AntiAlias;
        var bounds = new Rectangle(0, 0, Width - 1, Height - 1);
        using var path = LauncherTheme.RoundedRectangle(bounds, Compact ? 9 : 11);
        var baseColor = Danger
            ? Color.FromArgb(62, 23, 23)
            : Accent
                ? LauncherTheme.Bar
                : LauncherTheme.SurfaceRaised;
        var hoverColor = Danger
            ? Color.FromArgb(96, 30, 28)
            : Accent
                ? LauncherTheme.Red
                : LauncherTheme.SurfaceHover;
        var fillColor = !Enabled
            ? LauncherTheme.Surface
            : _pressed
                ? LauncherTheme.Blend(hoverColor, Color.Black, 0.15f)
                : _hovered
                    ? hoverColor
                    : baseColor;
        var outlineColor = Danger
            ? Color.FromArgb(126, 48, 44)
            : Accent
                ? LauncherTheme.BorderStrong
                : LauncherTheme.Border;
        using var fill = new SolidBrush(fillColor);
        using var outline = new Pen(outlineColor);
        graphics.FillPath(fill, path);
        graphics.DrawPath(outline, path);

        var foreground = Enabled ? LauncherTheme.Text : LauncherTheme.TextDim;
        var iconBounds = new Rectangle(Compact ? 10 : 13, (Height - 18) / 2, 18, 18);
        DrawIcon(graphics, iconBounds, foreground, Icon);
        var textLeft = Icon == RdrIcon.None ? 12 : iconBounds.Right + 9;
        using var buttonFont = new Font(Font, FontStyle.Bold);
        TextRenderer.DrawText(
            graphics,
            Text,
            buttonFont,
            new Rectangle(textLeft, 0, Width - textLeft - 10, Height),
            foreground,
            TextFormatFlags.Left | TextFormatFlags.VerticalCenter |
            TextFormatFlags.EndEllipsis);

        if (Focused)
        {
            ControlPaint.DrawFocusRectangle(graphics, Rectangle.Inflate(bounds, -4, -4));
        }
    }

    private static void DrawIcon(
        Graphics graphics,
        Rectangle bounds,
        Color color,
        RdrIcon icon)
    {
        if (icon == RdrIcon.None)
        {
            return;
        }

        using var pen = new Pen(color, 1.7f)
        {
            StartCap = LineCap.Round,
            EndCap = LineCap.Round,
            LineJoin = LineJoin.Round
        };
        var left = bounds.Left + 2;
        var top = bounds.Top + 2;
        var right = bounds.Right - 2;
        var bottom = bounds.Bottom - 2;
        var middleX = bounds.Left + (bounds.Width / 2);
        var middleY = bounds.Top + (bounds.Height / 2);

        switch (icon)
        {
            case RdrIcon.Home:
                graphics.DrawLines(pen,
                [
                    new Point(left, middleY),
                    new Point(middleX, top),
                    new Point(right, middleY)
                ]);
                graphics.DrawRectangle(pen, left + 2, middleY, bounds.Width - 8, bounds.Height / 2 - 2);
                break;
            case RdrIcon.Settings:
                graphics.DrawEllipse(pen, left + 3, top + 3, bounds.Width - 10, bounds.Height - 10);
                graphics.DrawEllipse(pen, left + 6, top + 6, bounds.Width - 16, bounds.Height - 16);
                graphics.DrawLine(pen, middleX, top, middleX, top + 4);
                graphics.DrawLine(pen, middleX, bottom - 4, middleX, bottom);
                graphics.DrawLine(pen, left, middleY, left + 4, middleY);
                graphics.DrawLine(pen, right - 4, middleY, right, middleY);
                break;
            case RdrIcon.Folder:
                graphics.DrawLines(pen,
                [
                    new Point(left, top + 4),
                    new Point(left + 5, top + 4),
                    new Point(left + 7, top + 7),
                    new Point(right, top + 7),
                    new Point(right, bottom),
                    new Point(left, bottom),
                    new Point(left, top + 4)
                ]);
                break;
            case RdrIcon.Search:
                graphics.DrawEllipse(pen, left, top, 9, 9);
                graphics.DrawLine(pen, left + 8, top + 8, right, bottom);
                break;
            case RdrIcon.Download:
                graphics.DrawLine(pen, middleX, top, middleX, bottom - 5);
                graphics.DrawLines(pen,
                [
                    new Point(middleX - 4, bottom - 9),
                    new Point(middleX, bottom - 5),
                    new Point(middleX + 4, bottom - 9)
                ]);
                graphics.DrawLine(pen, left, bottom, right, bottom);
                break;
            case RdrIcon.Trash:
                graphics.DrawRectangle(pen, left + 3, top + 5, bounds.Width - 10, bounds.Height - 8);
                graphics.DrawLine(pen, left + 1, top + 4, right - 1, top + 4);
                graphics.DrawLine(pen, middleX - 3, top + 1, middleX + 3, top + 1);
                break;
            case RdrIcon.Shield:
                graphics.DrawLines(pen,
                [
                    new Point(middleX, top),
                    new Point(right, top + 3),
                    new Point(right - 1, middleY + 3),
                    new Point(middleX, bottom),
                    new Point(left + 1, middleY + 3),
                    new Point(left, top + 3),
                    new Point(middleX, top)
                ]);
                break;
            case RdrIcon.Copy:
                graphics.DrawRectangle(pen, left + 4, top, bounds.Width - 8, bounds.Height - 8);
                graphics.DrawRectangle(pen, left, top + 4, bounds.Width - 8, bounds.Height - 8);
                break;
            case RdrIcon.Eye:
                graphics.DrawArc(pen, left, top + 2, bounds.Width - 4, bounds.Height - 5, 200, 140);
                graphics.DrawArc(pen, left, top + 3, bounds.Width - 4, bounds.Height - 5, 20, 140);
                graphics.DrawEllipse(pen, middleX - 2, middleY - 2, 4, 4);
                break;
            case RdrIcon.Link:
                graphics.DrawArc(pen, left, top + 4, 9, 8, 100, 250);
                graphics.DrawArc(pen, right - 9, top + 4, 9, 8, -80, 250);
                graphics.DrawLine(pen, middleX - 3, middleY, middleX + 3, middleY);
                break;
            case RdrIcon.Refresh:
                graphics.DrawArc(pen, left, top, bounds.Width - 4, bounds.Height - 4, 35, 285);
                graphics.DrawLines(pen,
                [
                    new Point(right - 1, top),
                    new Point(right, top + 5),
                    new Point(right - 5, top + 4)
                ]);
                break;
            case RdrIcon.Stop:
                graphics.DrawRectangle(pen, left + 2, top + 2, bounds.Width - 8, bounds.Height - 8);
                break;
            case RdrIcon.Log:
                graphics.DrawRectangle(pen, left + 2, top, bounds.Width - 8, bounds.Height - 4);
                graphics.DrawLine(pen, left + 5, top + 4, right - 3, top + 4);
                graphics.DrawLine(pen, left + 5, top + 8, right - 3, top + 8);
                graphics.DrawLine(pen, left + 5, top + 12, middleX + 2, top + 12);
                break;
            case RdrIcon.Arrow:
                graphics.DrawLine(pen, left, middleY, right, middleY);
                graphics.DrawLines(pen,
                [
                    new Point(right - 5, middleY - 5),
                    new Point(right, middleY),
                    new Point(right - 5, middleY + 5)
                ]);
                break;
        }
    }
}
