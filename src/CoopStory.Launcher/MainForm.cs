using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text.Json;
using Microsoft.Win32;
using CoopStory.Protocol;

namespace CoopStory.Launcher;

public sealed class MainForm : Form
{
    private const string DiagnosticsFileName = "RDR2-Coop-Diagnostics.zip";

    private readonly LauncherServices _services;
    private readonly ToolTip _toolTip = new();
    private readonly Panel _pageHost = new RdrBufferedPanel();
    private readonly TextBox _gamePath = CreateTextBox();
    private readonly TextBox _runtimePath = CreateTextBox();
    private readonly TextBox _diagnosticsFolder = CreateTextBox();
    private readonly TextBox _nickname = CreateTextBox();
    private readonly TextBox _hostAddress = CreateTextBox();
    private readonly TextBox _hostSave = CreateTextBox();
    private string _sessionToken = string.Empty;
    private readonly CheckBox _animGraphReplica = new();
    private readonly CheckBox _storyVmProbe = new();
    private readonly Label _status = new();
    private readonly Panel _statusDot = new();
    private readonly Label _sidecarState = new();
    private readonly Label _readiness = new();
    private readonly Label _startHint = new();
    private readonly Label _contextTitle = new();
    private readonly Label _contextDescription = new();
    private readonly Label _passwordState = new();
    private readonly Label _installationState = new();
    private readonly Label _diagnosticsLocation = new();
    private readonly RichTextBox _activityLog = new();
    private readonly List<RdrActionButton> _actionButtons = [];
    private readonly RdrModeCard _soloMode = new();
    private readonly RdrModeCard _hostMode = new();
    private readonly RdrModeCard _guestMode = new();
    private readonly RdrModeCard _steamPlatform = new();
    private readonly RdrModeCard _rockstarPlatform = new();
    private readonly RdrStartOrb _startOrb = new();
    private readonly RdrLobbyPanel _lobby = new();
    private readonly RdrActionButton _homeNav;
    private readonly RdrActionButton _settingsNav;
    private readonly RdrActionButton _stopButton;
    private readonly Control _soloContext;
    private readonly Control _multiplayerContext;
    private readonly RdrActionButton _detectAddressButton;
    private readonly Panel _homePage;
    private readonly Panel _settingsPage;
    private LauncherMode? _selectedMode;
    private LauncherPlatform? _selectedPlatform;
    private bool _busy;
    private bool _loading = true;
    private bool _showingSettings;
    private bool _passwordConfirmedForLaunch;
    private readonly HashSet<Control> _warmedPages = [];

    public MainForm(LauncherServices services)
    {
        _services = services;
        Text = "RDR2 Coop Story";
        StartPosition = FormStartPosition.CenterScreen;
        MinimumSize = new Size(1_100, 720);
        Size = new Size(1_340, 860);
        AutoScaleMode = AutoScaleMode.Dpi;
        Font = new Font("Segoe UI", 10f);
        BackColor = LauncherTheme.Background;
        ForeColor = LauncherTheme.Text;
        KeyPreview = true;
        DoubleBuffered = true;

        var root = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 1,
            RowCount = 3,
            BackColor = LauncherTheme.Background,
            Margin = Padding.Empty,
            Padding = Padding.Empty
        };
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 82));
        root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 38));
        Controls.Add(root);

        var header = CreateHeader(out _homeNav, out _settingsNav);
        root.Controls.Add(header, 0, 0);

        _pageHost.Dock = DockStyle.Fill;
        _pageHost.BackColor = LauncherTheme.Background;
        root.Controls.Add(_pageHost, 0, 1);

        _homePage = CreateHomePage(
            out _soloContext,
            out _multiplayerContext,
            out _detectAddressButton,
            out _stopButton);
        _settingsPage = CreateSettingsPage();
        _pageHost.Controls.Add(_settingsPage);
        _pageHost.Controls.Add(_homePage);
        root.Controls.Add(CreateFooter(), 0, 2);

        ConfigureEvents();
        LoadSettings();
        _loading = false;
        ShowPage(settings: false);
        UpdateContextUi();
        UpdateRunningUi();
        UpdateInstallationHint();
        UpdateReadiness();
        UpdateLobbyPreview();

        _services.Logger.LineWritten += LoggerOnLineWritten;
        _services.Sidecar.RunningChanged += SidecarOnRunningChanged;
        _services.Sidecar.LobbyChanged += SidecarOnLobbyChanged;
        FormClosing += OnFormClosing;
        KeyDown += (_, eventArgs) =>
        {
            if (eventArgs.Control && eventArgs.KeyCode == Keys.Oemcomma)
            {
                ShowPage(settings: true);
                eventArgs.Handled = true;
            }
            else if (eventArgs.KeyCode == Keys.Escape && _showingSettings)
            {
                ShowPage(settings: false);
                eventArgs.Handled = true;
            }
        };
    }

    protected override void OnHandleCreated(EventArgs eventArgs)
    {
        base.OnHandleCreated(eventArgs);
        TryEnableDarkTitleBar();
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            _toolTip.Dispose();
        }

        base.Dispose(disposing);
    }

    private Control CreateHeader(
        out RdrActionButton homeButton,
        out RdrActionButton settingsButton)
    {
        var header = new Panel
        {
            Dock = DockStyle.Fill,
            BackColor = LauncherTheme.BackgroundLift,
            Padding = new Padding(24, 13, 24, 11),
            Margin = Padding.Empty
        };
        var layout = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 2,
            RowCount = 1,
            BackColor = Color.Transparent,
            Margin = Padding.Empty
        };
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        header.Controls.Add(layout);

        var brand = new Panel
        {
            Dock = DockStyle.Fill,
            BackColor = Color.Transparent,
            Margin = Padding.Empty
        };
        var accent = new Panel
        {
            Dock = DockStyle.Left,
            Width = 5,
            BackColor = LauncherTheme.Red,
            Margin = Padding.Empty
        };
        var title = new Label
        {
            AutoSize = true,
            Location = new Point(18, 0),
            Font = CreateDisplayFont(23f),
            ForeColor = LauncherTheme.Text,
            Text = "RDR2  COOP STORY"
        };
        var build = new Label
        {
            AutoSize = true,
            Location = new Point(20, 39),
            Font = new Font(Font.FontFamily, 8.5f, FontStyle.Bold),
            ForeColor = LauncherTheme.TextMuted,
            Text = $"{ReadPackageDisplayName()}   •   LAUNCHER {ReadLauncherVersion()}"
        };
        brand.Controls.Add(build);
        brand.Controls.Add(title);
        brand.Controls.Add(accent);
        layout.Controls.Add(brand, 0, 0);

        var navigation = new FlowLayoutPanel
        {
            Dock = DockStyle.Fill,
            AutoSize = true,
            FlowDirection = FlowDirection.LeftToRight,
            WrapContents = false,
            BackColor = Color.Transparent,
            Margin = new Padding(0, 6, 0, 0)
        };
        homeButton = MakeButton("START", RdrIcon.Home, (_, _) => ShowPage(false));
        homeButton.Width = 120;
        homeButton.Accent = true;
        settingsButton = MakeButton(
            "USTAWIENIA",
            RdrIcon.Settings,
            (_, _) => ShowPage(true));
        settingsButton.Width = 150;
        navigation.Controls.Add(homeButton);
        navigation.Controls.Add(settingsButton);
        layout.Controls.Add(navigation, 1, 0);
        return header;
    }

    private Panel CreateHomePage(
        out Control soloContext,
        out Control multiplayerContext,
        out RdrActionButton detectAddressButton,
        out RdrActionButton stopButton)
    {
        var page = new RdrBackdrop
        {
            Dock = DockStyle.Fill,
            Padding = new Padding(24, 20, 24, 20),
            Margin = Padding.Empty
        };
        var columns = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 3,
            RowCount = 1,
            BackColor = Color.Transparent,
            Margin = Padding.Empty
        };
        columns.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 31));
        columns.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 29));
        columns.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 40));
        page.Controls.Add(columns);

        columns.Controls.Add(CreateModeSurface(), 0, 0);
        columns.Controls.Add(CreateLaunchSurface(), 1, 0);
        columns.Controls.Add(
            CreateSessionSurface(
                out soloContext,
                out multiplayerContext,
                out detectAddressButton,
                out stopButton),
            2,
            0);
        return page;
    }

    private Control CreateModeSurface()
    {
        var surface = CreateSurface(new Padding(18));
        surface.Margin = new Padding(0, 0, 9, 0);
        var layout = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 1,
            RowCount = 5,
            BackColor = Color.Transparent,
            Margin = Padding.Empty
        };
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 86));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 94));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 94));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 94));
        layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        surface.Controls.Add(layout);
        layout.Controls.Add(
            CreateSectionHeading(
                "01",
                "WYBIERZ TRYB",
                "Jeden launcher do szybkiego testu i sesji LAN."),
            0,
            0);

        ConfigureModeCard(
            _soloMode,
            "TEST SOLO",
            "Bot lokalny • najszybsza kontrola zmian",
            "1 PC",
            LauncherTheme.Red,
            LauncherMode.Solo);
        ConfigureModeCard(
            _hostMode,
            "HOSTUJ SESJĘ",
            "Tworzysz świat i zaproszenie dla znajomego",
            "HOST",
            Color.FromArgb(137, 53, 43),
            LauncherMode.Host);
        ConfigureModeCard(
            _guestMode,
            "DOŁĄCZAM",
            "Wpisujesz IPv4 hosta i jego hasło sesji",
            "GUEST",
            LauncherTheme.Guest,
            LauncherMode.Guest);
        layout.Controls.Add(_soloMode, 0, 1);
        layout.Controls.Add(_hostMode, 0, 2);
        layout.Controls.Add(_guestMode, 0, 3);

        var safety = new Label
        {
            Dock = DockStyle.Bottom,
            Height = 86,
            Text = "STORY MODE ONLY\nPrzed Red Dead Online zawsze odinstaluj mod.",
            Font = new Font(Font.FontFamily, 9f, FontStyle.Bold),
            ForeColor = LauncherTheme.Warning,
            TextAlign = ContentAlignment.BottomLeft,
            Padding = new Padding(4, 0, 8, 6)
        };
        layout.Controls.Add(safety, 0, 4);
        return surface;
    }

    private Control CreateLaunchSurface()
    {
        var surface = CreateSurface(new Padding(18));
        surface.Margin = new Padding(9, 0, 9, 0);
        var layout = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 1,
            RowCount = 5,
            BackColor = Color.Transparent,
            Margin = Padding.Empty
        };
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 86));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 82));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 82));
        layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 47));
        surface.Controls.Add(layout);
        layout.Controls.Add(
            CreateSectionHeading(
                "02",
                "PLATFORMA",
                "Kolor STARTU odpowiada wybranemu launcherowi."),
            0,
            0);

        ConfigurePlatformCard(
            _steamPlatform,
            "STEAM",
            "Uruchom przez steam://rungameid/1174180",
            LauncherTheme.Steam,
            LauncherPlatform.Steam);
        ConfigurePlatformCard(
            _rockstarPlatform,
            "ROCKSTAR",
            "Uruchom przez Rockstar Games Launcher",
            LauncherTheme.Rockstar,
            LauncherPlatform.Rockstar);
        layout.Controls.Add(_steamPlatform, 0, 1);
        layout.Controls.Add(_rockstarPlatform, 0, 2);

        _startOrb.Anchor = AnchorStyles.None;
        _startOrb.Font = Font;
        _startOrb.DisplayFont = CreateDisplayFont(21f);
        _startOrb.AccessibleName = "Uruchom wybrany tryb";
        _startOrb.AccessibleDescription =
            "Sprawdza instalację i uruchamia wybrany tryb przez wybraną platformę.";
        _startOrb.Click += async (_, _) =>
            await RunActionAsync(StartSelectedModeAsync);
        layout.Controls.Add(_startOrb, 0, 3);

        _startHint.Dock = DockStyle.Fill;
        _startHint.ForeColor = LauncherTheme.TextMuted;
        _startHint.Font = new Font(Font.FontFamily, 8.7f, FontStyle.Bold);
        _startHint.TextAlign = ContentAlignment.MiddleCenter;
        _startHint.Padding = new Padding(8, 0, 8, 0);
        layout.Controls.Add(_startHint, 0, 4);
        return surface;
    }

    private Control CreateSessionSurface(
        out Control soloContext,
        out Control multiplayerContext,
        out RdrActionButton detectAddressButton,
        out RdrActionButton stopButton)
    {
        var surface = CreateSurface(new Padding(18));
        surface.Margin = new Padding(9, 0, 0, 0);
        var layout = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 1,
            RowCount = 6,
            BackColor = Color.Transparent,
            Margin = Padding.Empty
        };
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 80));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 23));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 44));
        layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 62));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 62));
        surface.Controls.Add(layout);

        var heading = new Panel
        {
            Dock = DockStyle.Fill,
            BackColor = Color.Transparent,
            Margin = Padding.Empty
        };
        _contextTitle.AutoSize = true;
        _contextTitle.Location = new Point(0, 4);
        _contextTitle.Font = CreateDisplayFont(16f);
        _contextTitle.ForeColor = LauncherTheme.Text;
        _contextDescription.AutoSize = false;
        _contextDescription.Location = new Point(0, 34);
        _contextDescription.Size = new Size(500, 40);
        _contextDescription.Anchor = AnchorStyles.Left | AnchorStyles.Right | AnchorStyles.Top;
        _contextDescription.Font = new Font(Font.FontFamily, 9f);
        _contextDescription.ForeColor = LauncherTheme.TextMuted;
        heading.Controls.Add(_contextDescription);
        heading.Controls.Add(_contextTitle);
        layout.Controls.Add(heading, 0, 0);

        layout.Controls.Add(MakeFieldLabel("NICK W GRZE"), 0, 1);
        _nickname.MaxLength = 48;
        _nickname.PlaceholderText = "np. ArthurPL";
        layout.Controls.Add(_nickname, 0, 2);

        var contextHost = new Panel
        {
            Dock = DockStyle.Fill,
            BackColor = Color.Transparent,
            AutoScroll = true,
            Padding = new Padding(0, 8, 0, 3),
            Margin = Padding.Empty
        };
        soloContext = CreateSoloContext();
        multiplayerContext = CreateMultiplayerContext(
            out detectAddressButton);
        contextHost.Controls.Add(multiplayerContext);
        contextHost.Controls.Add(soloContext);
        layout.Controls.Add(contextHost, 0, 3);

        var readinessPanel = new Panel
        {
            Dock = DockStyle.Fill,
            BackColor = LauncherTheme.BackgroundLift,
            Padding = new Padding(12, 8, 12, 6),
            Margin = new Padding(0, 5, 0, 5)
        };
        _readiness.Dock = DockStyle.Fill;
        _readiness.Font = new Font(Font.FontFamily, 8.5f, FontStyle.Bold);
        _readiness.ForeColor = LauncherTheme.TextMuted;
        _readiness.TextAlign = ContentAlignment.MiddleLeft;
        readinessPanel.Controls.Add(_readiness);
        layout.Controls.Add(readinessPanel, 0, 4);

        var quickActions = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 3,
            RowCount = 1,
            BackColor = LauncherTheme.Bar,
            Padding = new Padding(8, 8, 8, 8),
            Margin = Padding.Empty
        };
        quickActions.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        quickActions.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 134));
        quickActions.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 150));
        var diagnosticCaption = new Label
        {
            Dock = DockStyle.Fill,
            Text = "DIAGNOSTYKA\n1 klik • stary ZIP zostanie zastąpiony",
            ForeColor = LauncherTheme.Text,
            Font = new Font(Font.FontFamily, 8.2f, FontStyle.Bold),
            TextAlign = ContentAlignment.MiddleLeft
        };
        var exportDiagnostics = MakeButton(
            "EKSPORTUJ",
            RdrIcon.Download,
            async (_, _) => await RunActionAsync(ExportDiagnosticsAsync));
        exportDiagnostics.Dock = DockStyle.Fill;
        exportDiagnostics.Margin = new Padding(4, 0, 4, 0);
        exportDiagnostics.Accent = true;
        exportDiagnostics.Compact = true;
        stopButton = MakeButton(
            "WYŁĄCZ COOP",
            RdrIcon.Stop,
            (_, _) => StopSession());
        stopButton.Dock = DockStyle.Fill;
        stopButton.Margin = new Padding(4, 0, 0, 0);
        stopButton.Danger = true;
        stopButton.Compact = true;
        quickActions.Controls.Add(diagnosticCaption, 0, 0);
        quickActions.Controls.Add(exportDiagnostics, 1, 0);
        quickActions.Controls.Add(stopButton, 2, 0);
        layout.Controls.Add(quickActions, 0, 5);
        return surface;
    }

    private Control CreateSoloContext()
    {
        var panel = new Panel
        {
            Dock = DockStyle.Top,
            Height = 176,
            BackColor = Color.Transparent,
            Margin = Padding.Empty
        };
        var icon = new Label
        {
            AutoSize = true,
            Location = new Point(0, 12),
            Font = new Font("Segoe UI Symbol", 25f),
            ForeColor = LauncherTheme.RedBright,
            Text = "◎"
        };
        var title = new Label
        {
            AutoSize = true,
            Location = new Point(48, 13),
            Font = new Font(Font.FontFamily, 11f, FontStyle.Bold),
            ForeColor = LauncherTheme.Text,
            Text = "Szybki test lokalny"
        };
        var description = new Label
        {
            AutoSize = false,
            Location = new Point(49, 42),
            Anchor = AnchorStyles.Left | AnchorStyles.Right | AnchorStyles.Top,
            Size = new Size(430, 104),
            Font = new Font(Font.FontFamily, 9f),
            ForeColor = LauncherTheme.TextMuted,
            Text =
                "Launcher sprawdzi instalację, w razie potrzeby podmieni bieżący build " +
                "i uruchomi syntetycznego gracza SOLO BOT. Po wejściu do Story Mode " +
                "otwórz F9 i wybierz „Test solo: start / stop”."
        };
        panel.Controls.Add(description);
        panel.Controls.Add(title);
        panel.Controls.Add(icon);
        return panel;
    }

    private Control CreateMultiplayerContext(
        out RdrActionButton detectAddressButton)
    {
        var panel = new TableLayoutPanel
        {
            Dock = DockStyle.Top,
            AutoSize = true,
            ColumnCount = 1,
            RowCount = 4,
            BackColor = Color.Transparent,
            Margin = Padding.Empty
        };
        panel.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        panel.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        panel.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        panel.RowStyles.Add(new RowStyle(SizeType.AutoSize));

        _lobby.Dock = DockStyle.Top;
        _lobby.Margin = new Padding(0, 0, 0, 10);
        panel.Controls.Add(_lobby, 0, 0);

        detectAddressButton = MakeButton(
            "WYKRYJ",
            RdrIcon.Search,
            DetectHostAddress);
        detectAddressButton.Width = 112;
        var addressBlock = CreateFieldBlock(
            "IPV4 SESJI — HOST: SWÓJ / GUEST: ADRES HOSTA",
            _hostAddress,
            detectAddressButton);
        _hostAddress.PlaceholderText = "np. Hamachi 25.x.x.x albo LAN 192.168.x.x";
        panel.Controls.Add(addressBlock, 0, 1);

        var passwordPanel = new Panel
        {
            Dock = DockStyle.Top,
            Height = 62,
            BackColor = LauncherTheme.BackgroundLift,
            Padding = new Padding(13, 8, 13, 8),
            Margin = new Padding(0, 7, 0, 5)
        };
        _passwordState.Dock = DockStyle.Fill;
        _passwordState.Font = new Font(Font.FontFamily, 8.5f, FontStyle.Bold);
        _passwordState.ForeColor = LauncherTheme.Warning;
        _passwordState.TextAlign = ContentAlignment.MiddleLeft;
        passwordPanel.Controls.Add(_passwordState);
        panel.Controls.Add(passwordPanel, 0, 2);

        var note = new Label
        {
            Dock = DockStyle.Top,
            AutoSize = true,
            MaximumSize = new Size(470, 0),
            Margin = new Padding(0, 4, 0, 8),
            ForeColor = LauncherTheme.TextDim,
            Font = new Font(Font.FontFamily, 8.2f),
            Text =
                "Hasło podajesz dopiero po kliknięciu HOSTUJ lub DOŁĄCZ. " +
                "Nie jest zapisywane jawnie ani dodawane do diagnostyki."
        };
        panel.Controls.Add(note, 0, 3);
        return panel;
    }

    private Panel CreateSettingsPage()
    {
        var page = new RdrBackdrop
        {
            Dock = DockStyle.Fill,
            Padding = new Padding(24, 20, 24, 20),
            Margin = Padding.Empty,
            Visible = false
        };
        var columns = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 2,
            RowCount = 1,
            BackColor = Color.Transparent,
            Margin = Padding.Empty
        };
        columns.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 57));
        columns.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 43));
        page.Controls.Add(columns);
        columns.Controls.Add(CreatePathsSurface(), 0, 0);
        columns.Controls.Add(CreateMaintenanceColumn(), 1, 0);
        return page;
    }

    private Control CreatePathsSurface()
    {
        var surface = CreateSurface(new Padding(22));
        surface.Margin = new Padding(0, 0, 10, 0);
        var layout = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 1,
            RowCount = 9,
            BackColor = Color.Transparent,
            Margin = Padding.Empty
        };
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 76));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 72));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 72));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 72));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 72));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 84));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 54));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 82));
        layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        surface.Controls.Add(layout);
        layout.Controls.Add(
            CreateSectionHeading(
                "",
                "USTAWIENIA MODA",
                "Ścieżki i silnik ruchu zapisują się lokalnie między buildami."),
            0,
            0);

        var browseGame = MakeButton(
            "PRZEGLĄDAJ",
            RdrIcon.Folder,
            BrowseGame);
        browseGame.Width = 132;
        layout.Controls.Add(
            CreateFieldBlock("PLIK GRY — RDR2.EXE", _gamePath, browseGame),
            0,
            1);

        var browseRuntime = MakeButton(
            "PRZEGLĄDAJ",
            RdrIcon.Folder,
            BrowseRuntime);
        browseRuntime.Width = 132;
        layout.Controls.Add(
            CreateFieldBlock(
                "ROZPAKOWANY SCRIPTHOOK — FOLDER LUB BIN",
                _runtimePath,
                browseRuntime),
            0,
            2);

        var browseHostSave = MakeButton(
            "WYBIERZ",
            RdrIcon.Folder,
            BrowseHostSave);
        browseHostSave.Width = 132;
        _hostSave.ReadOnly = true;
        layout.Controls.Add(
            CreateFieldBlock(
                "LOKALNY SAVE HOSTA — SRDR*",
                _hostSave,
                browseHostSave),
            0,
            3);

        var browseDiagnostics = MakeButton(
            "PRZEGLĄDAJ",
            RdrIcon.Folder,
            BrowseDiagnosticsFolder);
        browseDiagnostics.Width = 132;
        layout.Controls.Add(
            CreateFieldBlock(
                "FOLDER EKSPORTU DIAGNOSTYKI",
                _diagnosticsFolder,
                browseDiagnostics),
            0,
            4);

        layout.Controls.Add(CreateMotionReplicationSelector(), 0, 5);

        var pathActions = new FlowLayoutPanel
        {
            Dock = DockStyle.Fill,
            AutoSize = false,
            FlowDirection = FlowDirection.LeftToRight,
            WrapContents = false,
            BackColor = Color.Transparent,
            Margin = new Padding(0, 7, 0, 7)
        };
        var detect = MakeButton(
            "WYKRYJ TYPOWE ŚCIEŻKI",
            RdrIcon.Search,
            (_, _) => DetectCommonPaths());
        detect.Width = 210;
        var save = MakeButton(
            "ZAPISZ USTAWIENIA",
            RdrIcon.Shield,
            (_, _) => SaveSettingsWithFeedback());
        save.Width = 196;
        save.Accent = true;
        pathActions.Controls.Add(detect);
        pathActions.Controls.Add(save);
        layout.Controls.Add(pathActions, 0, 6);

        var diagnosticsNote = new Panel
        {
            Dock = DockStyle.Fill,
            BackColor = LauncherTheme.BackgroundLift,
            Padding = new Padding(14, 11, 14, 9),
            Margin = new Padding(0, 6, 0, 6)
        };
        _diagnosticsLocation.Dock = DockStyle.Fill;
        _diagnosticsLocation.ForeColor = LauncherTheme.TextMuted;
        _diagnosticsLocation.Font = new Font(Font.FontFamily, 8.8f);
        _diagnosticsLocation.TextAlign = ContentAlignment.MiddleLeft;
        diagnosticsNote.Controls.Add(_diagnosticsLocation);
        layout.Controls.Add(diagnosticsNote, 0, 7);

        var tip = new Label
        {
            Dock = DockStyle.Top,
            Height = 90,
            ForeColor = LauncherTheme.TextDim,
            Font = new Font(Font.FontFamily, 8.5f),
            Padding = new Padding(2, 8, 8, 0),
            Text =
                "QoL: START sam sprawdza instalację i instaluje aktualny build, jeśli trzeba. " +
                "ScriptHook nie jest dołączany do paczki i nadal musi być wskazany osobno. " +
                "Ustawienia są w LocalAppData, więc nowe paczki release nie zerują ścieżek."
        };
        layout.Controls.Add(tip, 0, 8);
        return surface;
    }

    private Control CreateMotionReplicationSelector()
    {
        var panel = new Panel
        {
            Dock = DockStyle.Fill,
            BackColor = LauncherTheme.BackgroundLift,
            Padding = new Padding(14, 9, 14, 8),
            Margin = new Padding(0, 5, 0, 5)
        };
        var layout = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 2,
            RowCount = 2,
            BackColor = Color.Transparent,
            Margin = Padding.Empty
        };
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 206));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 28));
        layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        panel.Controls.Add(layout);

        var title = new Label
        {
            Dock = DockStyle.Fill,
            Text = "SILNIK RUCHU ZDALNEJ POSTACI",
            Font = new Font(Font.FontFamily, 8.8f, FontStyle.Bold),
            ForeColor = LauncherTheme.Text,
            TextAlign = ContentAlignment.MiddleLeft
        };
        var description = new Label
        {
            Dock = DockStyle.Fill,
            Text =
                "Wyłączone: Task/Navmesh. Włączone: bezpośrednia replika ruchu bez navmeshu; w 2P ustaw tak samo na obu PC.",
            Font = new Font(Font.FontFamily, 8.1f),
            ForeColor = LauncherTheme.TextDim,
            TextAlign = ContentAlignment.TopLeft
        };
        static void ConfigureToggle(
            CheckBox checkBox,
            string text,
            Font font)
        {
            checkBox.Dock = DockStyle.Fill;
            checkBox.AutoSize = false;
            checkBox.CheckAlign = ContentAlignment.MiddleLeft;
            checkBox.TextAlign = ContentAlignment.MiddleLeft;
            checkBox.Padding = new Padding(8, 0, 0, 0);
            checkBox.Margin = new Padding(0, 0, 0, 3);
            checkBox.FlatStyle = FlatStyle.Flat;
            checkBox.FlatAppearance.BorderColor = LauncherTheme.BorderStrong;
            checkBox.FlatAppearance.CheckedBackColor = LauncherTheme.Red;
            checkBox.BackColor = LauncherTheme.Background;
            checkBox.ForeColor = LauncherTheme.Text;
            checkBox.Font = font;
            checkBox.Text = text;
        }

        var toggleFont = new Font(
            Font.FontFamily,
            8.2f,
            FontStyle.Bold);
        ConfigureToggle(
            _animGraphReplica,
            "ANIMGRAPH REPLICA",
            toggleFont);
        _animGraphReplica.AccessibleName =
            "Eksperymentalny silnik AnimGraph Replica";
        _animGraphReplica.AccessibleDescription =
            "Osobny eksperymentalny silnik. Po wyłączeniu launcher używa dotychczasowego Task/Navmesh.";

        ConfigureToggle(
            _storyVmProbe,
            "STORY VM CAPTURE",
            toggleFont);
        _storyVmProbe.AccessibleName =
            "Eksperymentalne przechwytywanie Story VM";
        _storyVmProbe.AccessibleDescription =
            "Eksperymentalnie przechwytuje definicje AnimScene na przypiętej wersji gry. Domyślnie wyłączone.";

        var toggles = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 1,
            RowCount = 2,
            BackColor = Color.Transparent,
            Margin = Padding.Empty
        };
        toggles.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        toggles.RowStyles.Add(new RowStyle(SizeType.Percent, 50));
        toggles.RowStyles.Add(new RowStyle(SizeType.Percent, 50));
        toggles.Controls.Add(_animGraphReplica, 0, 0);
        toggles.Controls.Add(_storyVmProbe, 0, 1);

        layout.Controls.Add(title, 0, 0);
        layout.SetColumnSpan(title, 1);
        layout.Controls.Add(description, 0, 1);
        layout.Controls.Add(toggles, 1, 0);
        layout.SetRowSpan(toggles, 2);
        _toolTip.SetToolTip(
            _animGraphReplica,
            "Bezpośrednio replikuje transform i dostępny stan ruchu. Pełne warstwy i clipy AnimGraphu nie są jeszcze odczytywane. Zmiana działa przy następnym START.");
        _toolTip.SetToolTip(
            _storyVmProbe,
            "Eksperymentalne: przechwytuje zasób, playback i role Story VM, aby odtworzyć dokładną AnimScene guesta. Działa tylko na przypiętym buildzie i wyłącza się bezpiecznie przy niezgodności. Zmiana działa przy następnym START.");
        return panel;
    }

    private Control CreateMaintenanceColumn()
    {
        var column = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 1,
            RowCount = 2,
            BackColor = Color.Transparent,
            Margin = new Padding(10, 0, 0, 0)
        };
        column.RowStyles.Add(new RowStyle(SizeType.Absolute, 326));
        column.RowStyles.Add(new RowStyle(SizeType.Percent, 100));

        var maintenance = CreateSurface(new Padding(18));
        maintenance.Margin = new Padding(0, 0, 0, 9);
        var maintenanceLayout = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 1,
            RowCount = 5,
            BackColor = Color.Transparent,
            Margin = Padding.Empty
        };
        maintenanceLayout.RowStyles.Add(new RowStyle(SizeType.Absolute, 73));
        maintenanceLayout.RowStyles.Add(new RowStyle(SizeType.Absolute, 58));
        maintenanceLayout.RowStyles.Add(new RowStyle(SizeType.Absolute, 58));
        maintenanceLayout.RowStyles.Add(new RowStyle(SizeType.Absolute, 58));
        maintenanceLayout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        maintenance.Controls.Add(maintenanceLayout);
        maintenanceLayout.Controls.Add(
            CreateSectionHeading(
                "",
                "INSTALACJA I SERWIS",
                "Bezpieczna kontrola plików bieżącego builda."),
            0,
            0);

        _installationState.Dock = DockStyle.Fill;
        _installationState.BackColor = LauncherTheme.BackgroundLift;
        _installationState.ForeColor = LauncherTheme.TextMuted;
        _installationState.Padding = new Padding(12, 0, 12, 0);
        _installationState.TextAlign = ContentAlignment.MiddleLeft;
        _installationState.Font = new Font(Font.FontFamily, 8.7f, FontStyle.Bold);
        maintenanceLayout.Controls.Add(_installationState, 0, 1);

        var installActions = new FlowLayoutPanel
        {
            Dock = DockStyle.Fill,
            FlowDirection = FlowDirection.LeftToRight,
            WrapContents = false,
            BackColor = Color.Transparent,
            Margin = new Padding(0, 8, 0, 4)
        };
        var verify = MakeButton(
            "SPRAWDŹ",
            RdrIcon.Shield,
            async (_, _) => await RunActionAsync(VerifyAsync));
        verify.Width = 122;
        var install = MakeButton(
            "ZAINSTALUJ",
            RdrIcon.Download,
            async (_, _) => await RunActionAsync(InstallAsync));
        install.Width = 145;
        install.Accent = true;
        var uninstall = MakeButton(
            "ODINSTALUJ",
            RdrIcon.Trash,
            async (_, _) => await RunActionAsync(UninstallAsync));
        uninstall.Width = 143;
        uninstall.Danger = true;
        installActions.Controls.Add(verify);
        installActions.Controls.Add(install);
        installActions.Controls.Add(uninstall);
        maintenanceLayout.Controls.Add(installActions, 0, 2);

        var folderActions = new FlowLayoutPanel
        {
            Dock = DockStyle.Fill,
            FlowDirection = FlowDirection.LeftToRight,
            WrapContents = false,
            BackColor = Color.Transparent,
            Margin = new Padding(0, 8, 0, 4)
        };
        var openLogs = MakeButton(
            "OTWÓRZ LOGI",
            RdrIcon.Log,
            (_, _) => SafeUiAction(OpenLogs));
        openLogs.Width = 145;
        var openDiagnostics = MakeButton(
            "FOLDER DIAGNOSTYKI",
            RdrIcon.Folder,
            (_, _) => SafeUiAction(OpenDiagnosticsFolder));
        openDiagnostics.Width = 205;
        folderActions.Controls.Add(openLogs);
        folderActions.Controls.Add(openDiagnostics);
        maintenanceLayout.Controls.Add(folderActions, 0, 3);

        var version = new Label
        {
            Dock = DockStyle.Fill,
            ForeColor = LauncherTheme.TextDim,
            Font = new Font(Font.FontFamily, 8.2f),
            TextAlign = ContentAlignment.BottomLeft,
            Text =
                $"Paczka: {ReadPackageDisplayName()}\n" +
                $"Launcher: {ReadLauncherVersion()} • protokół i build odczytywane z paczki release"
        };
        maintenanceLayout.Controls.Add(version, 0, 4);
        column.Controls.Add(maintenance, 0, 0);

        var activity = CreateSurface(new Padding(16));
        activity.Margin = new Padding(0, 9, 0, 0);
        var activityLayout = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 1,
            RowCount = 2,
            BackColor = Color.Transparent,
            Margin = Padding.Empty
        };
        activityLayout.RowStyles.Add(new RowStyle(SizeType.Absolute, 42));
        activityLayout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        activity.Controls.Add(activityLayout);
        var activityTitle = new Label
        {
            Dock = DockStyle.Fill,
            Text = "OSTATNIA AKTYWNOŚĆ",
            Font = new Font(Font.FontFamily, 9.5f, FontStyle.Bold),
            ForeColor = LauncherTheme.Text,
            TextAlign = ContentAlignment.MiddleLeft
        };
        _activityLog.Dock = DockStyle.Fill;
        _activityLog.ReadOnly = true;
        _activityLog.BorderStyle = BorderStyle.None;
        _activityLog.BackColor = LauncherTheme.Background;
        _activityLog.ForeColor = LauncherTheme.TextMuted;
        _activityLog.Font = new Font("Cascadia Mono", 8.4f);
        _activityLog.Text =
            $"{DateTime.Now:HH:mm:ss}  Launcher gotowy. Wybierz tryb i platformę.\n";
        activityLayout.Controls.Add(activityTitle, 0, 0);
        activityLayout.Controls.Add(_activityLog, 0, 1);
        column.Controls.Add(activity, 0, 1);
        return column;
    }

    private Control CreateFooter()
    {
        var footer = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 3,
            RowCount = 1,
            BackColor = LauncherTheme.BackgroundLift,
            Padding = new Padding(24, 0, 24, 0),
            Margin = Padding.Empty
        };
        footer.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 22));
        footer.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        footer.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 190));
        _statusDot.Size = new Size(8, 8);
        _statusDot.Anchor = AnchorStyles.None;
        _statusDot.BackColor = LauncherTheme.TextDim;
        _status.Dock = DockStyle.Fill;
        _status.TextAlign = ContentAlignment.MiddleLeft;
        _status.Font = new Font(Font.FontFamily, 8.5f, FontStyle.Bold);
        _status.ForeColor = LauncherTheme.TextMuted;
        _status.Text = "Gotowy.";
        _sidecarState.Dock = DockStyle.Fill;
        _sidecarState.TextAlign = ContentAlignment.MiddleRight;
        _sidecarState.Font = new Font(Font.FontFamily, 8.5f, FontStyle.Bold);
        _sidecarState.ForeColor = LauncherTheme.TextDim;
        footer.Controls.Add(_statusDot, 0, 0);
        footer.Controls.Add(_status, 1, 0);
        footer.Controls.Add(_sidecarState, 2, 0);
        return footer;
    }

    private void ConfigureEvents()
    {
        foreach (var textBox in new[]
                 {
                     _gamePath,
                     _runtimePath,
                     _diagnosticsFolder,
                     _nickname,
                     _hostAddress,
                     _hostSave
                 })
        {
            textBox.TextChanged += (_, _) =>
            {
                if (!_loading)
                {
                    UpdateReadiness();
                    UpdateLobbyPreview();
                }
            };
        }

        _diagnosticsFolder.TextChanged += (_, _) => UpdateDiagnosticsLocation();
        _animGraphReplica.CheckedChanged += (_, _) =>
        {
            if (!_loading)
            {
                SaveSettingsSilently();
                SetStatus(
                    _animGraphReplica.Checked
                        ? "AnimGraph Replica zostanie użyty przy następnym uruchomieniu."
                        : "Przywrócono silnik Task/Navmesh przy następnym uruchomieniu.",
                StatusKind.Neutral);
            }
        };
        _storyVmProbe.CheckedChanged += (_, _) =>
        {
            if (!_loading)
            {
                SaveSettingsSilently();
                SetStatus(
                    _storyVmProbe.Checked
                        ? "Story VM Capture zostanie uruchomiony przy następnym START (eksperymentalne)."
                        : "Story VM Capture jest wyłączony przy następnym START; pozostaje bezpieczny fallback.",
                    StatusKind.Neutral);
            }
        };
        _toolTip.SetToolTip(
            _startOrb,
            "Launcher sprawdzi pliki, zainstaluje bieżący build i uruchomi wybrany tryb.");
    }

    private void ConfigureModeCard(
        RdrModeCard card,
        string title,
        string description,
        string badge,
        Color accent,
        LauncherMode mode)
    {
        card.Dock = DockStyle.Fill;
        card.Margin = new Padding(0, 4, 0, 6);
        card.Font = new Font(Font.FontFamily, 9f);
        card.TitleText = title;
        card.DescriptionText = description;
        card.BadgeText = badge;
        card.AccentColor = accent;
        card.Text = title;
        card.AccessibleName = title;
        card.AccessibleDescription = description;
        card.Click += (_, _) => SelectMode(mode);
    }

    private void ConfigurePlatformCard(
        RdrModeCard card,
        string title,
        string description,
        Color accent,
        LauncherPlatform platform)
    {
        card.Dock = DockStyle.Fill;
        card.Margin = new Padding(0, 4, 0, 6);
        card.Font = new Font(Font.FontFamily, 8.5f);
        card.TitleText = title;
        card.DescriptionText = description;
        card.AccentColor = accent;
        card.Text = title;
        card.AccessibleName = title;
        card.AccessibleDescription = description;
        card.Click += (_, _) => SelectPlatform(platform);
    }

    private void SelectMode(LauncherMode mode)
    {
        _selectedMode = mode;
        _passwordConfirmedForLaunch = false;
        if (mode == LauncherMode.Host)
        {
            if (string.IsNullOrWhiteSpace(_hostAddress.Text))
            {
                _hostAddress.Text = InviteService.SuggestedLanAddress();
            }
        }

        UpdateContextUi();
        UpdatePasswordState();
        UpdateReadiness();
        UpdateLobbyPreview();
        SaveSettingsSilently();
    }

    private void SelectPlatform(LauncherPlatform platform)
    {
        _selectedPlatform = platform;
        UpdateContextUi();
        UpdateReadiness();
        SaveSettingsSilently();
    }

    private void UpdateContextUi()
    {
        _soloMode.Selected = _selectedMode == LauncherMode.Solo;
        _hostMode.Selected = _selectedMode == LauncherMode.Host;
        _guestMode.Selected = _selectedMode == LauncherMode.Guest;
        _steamPlatform.Selected = _selectedPlatform == LauncherPlatform.Steam;
        _rockstarPlatform.Selected = _selectedPlatform == LauncherPlatform.Rockstar;

        var isSolo = _selectedMode == LauncherMode.Solo;
        var isHost = _selectedMode == LauncherMode.Host;
        var isGuest = _selectedMode == LauncherMode.Guest;
        _soloContext.Visible = isSolo;
        _multiplayerContext.Visible = isHost || isGuest;
        _detectAddressButton.Visible = isHost;
        _hostAddress.ReadOnly = false;

        (_contextTitle.Text, _contextDescription.Text) = _selectedMode switch
        {
            LauncherMode.Solo => (
                "TEST SOLO",
                "Najszybszy test zmian silnika na jednym komputerze."),
            LauncherMode.Host => (
                "HOST SESJI",
                "Kliknij HOSTUJ, ustaw hasło sesji i poczekaj na drugiego gracza."),
            LauncherMode.Guest => (
                "DOŁĄCZANIE",
                "Kliknij DOŁĄCZ i podaj IPv4 hosta oraz jego hasło sesji."),
            _ => (
                "KONFIGURACJA SESJI",
                "Najpierw wybierz rodzaj testu po lewej stronie.")
        };
        UpdatePasswordState();
    }

    private void UpdatePasswordState()
    {
        if (_selectedMode is not (LauncherMode.Host or LauncherMode.Guest))
        {
            return;
        }

        _passwordState.Text = _passwordConfirmedForLaunch
            ? "✓  HASŁO ZAPISANE — GOTOWE DO UWIERZYTELNIENIA"
            : _selectedMode == LauncherMode.Host
                ? "HASŁO SESJI — PODASZ JE PO KLIKNIĘCIU HOSTUJ"
                : "IPV4 + HASŁO — PODASZ JE PO KLIKNIĘCIU DOŁĄCZ";
        _passwordState.ForeColor = _passwordConfirmedForLaunch
            ? LauncherTheme.Success
            : LauncherTheme.Warning;
    }

    private void UpdateReadiness()
    {
        if (_loading)
        {
            return;
        }

        var issues = new List<string>();
        if (_selectedMode is null)
        {
            issues.Add("wybierz tryb");
        }

        if (_selectedPlatform is null)
        {
            issues.Add("wybierz platformę");
        }

        if (!IsNicknameValid(_nickname.Text))
        {
            issues.Add("uzupełnij poprawny nick");
        }

        if (!File.Exists(_gamePath.Text.Trim()))
        {
            issues.Add("wskaż RDR2.exe w Ustawieniach");
        }

        if (!IsRuntimeFolderValid(_runtimePath.Text.Trim()))
        {
            issues.Add("wskaż ScriptHook w Ustawieniach");
        }

        if (_selectedMode == LauncherMode.Host)
        {
            if (!IsHostAddressValid(_hostAddress.Text))
            {
                issues.Add("wykryj adres hosta");
            }

            if (!IsSavePathValid(_hostSave.Text))
            {
                issues.Add("wybierz save SRDR* w Ustawieniach");
            }
        }
        // Guest may press DOŁĄCZ before entering the connection data. The
        // themed lobby prompt asks for IPv4 and the session password then.

        var sessionReady = _selectedMode switch
        {
            LauncherMode.Solo => true,
            LauncherMode.Host =>
                IsHostAddressValid(_hostAddress.Text) &&
                IsSavePathValid(_hostSave.Text),
            LauncherMode.Guest => true,
            _ => false
        };
        var sidecarRunning = _services.Sidecar.IsRunning;
        var ready = issues.Count == 0 && !_busy && !sidecarRunning;
        _startOrb.Enabled = ready;
        _startOrb.Busy = _busy;
        _startOrb.AccentColor = _selectedPlatform switch
        {
            LauncherPlatform.Steam => LauncherTheme.Steam,
            LauncherPlatform.Rockstar => LauncherTheme.Rockstar,
            _ => LauncherTheme.Red
        };
        _startOrb.MainText = _selectedMode switch
        {
            LauncherMode.Solo => "TESTUJ",
            LauncherMode.Host => "HOSTUJ",
            LauncherMode.Guest => "DOŁĄCZ",
            _ => "START"
        };
        _startOrb.DetailText = _selectedPlatform switch
        {
            LauncherPlatform.Steam => "PRZEZ STEAM",
            LauncherPlatform.Rockstar => "PRZEZ ROCKSTAR",
            _ => "WYBIERZ PLATFORMĘ"
        };
        _startOrb.Invalidate();

        if (sidecarRunning)
        {
            _startHint.Text = "SESJA DZIAŁA — UŻYJ STOP PO TEŚCIE";
            _startHint.ForeColor = LauncherTheme.Success;
        }
        else if (ready)
        {
            _startHint.Text = _selectedMode switch
            {
                LauncherMode.Host => "HOSTUJ • USTAW HASŁO SESJI",
                LauncherMode.Guest => "DOŁĄCZ • PODAJ IPV4 I HASŁO",
                _ => "GOTOWE • START SPRAWDZI I ZAINSTALUJE BUILD"
            };
            _startHint.ForeColor = _selectedMode is LauncherMode.Host or LauncherMode.Guest
                ? LauncherTheme.Warning
                : LauncherTheme.Success;
        }
        else
        {
            _startHint.Text = issues.Count == 0
                ? "CHWILA — TRWA OPERACJA"
                : issues[0].ToUpperInvariant();
            _startHint.ForeColor = LauncherTheme.TextMuted;
        }

        var selectionReady = _selectedMode is not null && _selectedPlatform is not null;
        var pathsReady = File.Exists(_gamePath.Text.Trim()) &&
                         IsRuntimeFolderValid(_runtimePath.Text.Trim());
        _readiness.Text =
            $"{ReadyMark(selectionReady)} tryb + platforma    " +
            $"{ReadyMark(IsNicknameValid(_nickname.Text))} nick\n" +
            $"{ReadyMark(pathsReady)} pliki gry    " +
            $"{ReadyMark(sessionReady)} dane przed startem";
        _readiness.ForeColor = ready
            ? LauncherTheme.Success
            : LauncherTheme.TextMuted;

        foreach (var button in _actionButtons)
        {
            button.Enabled = !_busy;
        }

        _stopButton.Enabled = !_busy && sidecarRunning;
        UpdateDiagnosticsLocation();
    }

    private async Task StartSelectedModeAsync()
    {
        if (_selectedMode is null || _selectedPlatform is null)
        {
            throw new LauncherException("Wybierz tryb oraz platformę startu.");
        }

        string? enteredPassword = null;
        if (_selectedMode == LauncherMode.Host &&
            !PromptHostPassword(out enteredPassword))
        {
            return;
        }
        if (_selectedMode == LauncherMode.Guest &&
            !PromptGuestConnection(out enteredPassword))
        {
            return;
        }

        if (_selectedMode is LauncherMode.Host or LauncherMode.Guest)
        {
            var password = enteredPassword ?? throw new LauncherException(
                "Nie podano hasła sesji.");
            var hostAddress = _hostAddress.Text.Trim();
            _sessionToken = await Task.Run(() =>
                SessionPasswordService.DeriveSessionToken(
                    password,
                    hostAddress));
            enteredPassword = null;
            _passwordConfirmedForLaunch = true;
            UpdatePasswordState();
            SaveSettingsSilently();
            SetStatus(
                _selectedMode == LauncherMode.Host
                    ? "Hasło zapisane. Sesja HOST jest gotowa do uruchomienia."
                    : "Hasło zapisane. Dane GUEST są gotowe do połączenia.",
                StatusKind.Success);
        }

        SaveSettings();
        var settings = ReadSettings();
        var launchTarget = _selectedPlatform == LauncherPlatform.Rockstar
            ? GameLaunchTarget.Rockstar
            : GameLaunchTarget.Steam;
        var modeName = _selectedMode switch
        {
            LauncherMode.Solo => "TEST SOLO",
            LauncherMode.Host => "HOST",
            LauncherMode.Guest => "CLIENT",
            _ => "TEST"
        };
        var platformName = launchTarget == GameLaunchTarget.Steam
            ? "Steam"
            : "Rockstar Games Launcher";
        var effectiveSettings = _selectedMode == LauncherMode.Solo
            ? settings with
            {
                Role = LauncherRole.Host,
                HostSavePath = string.Empty
            }
            : settings;
        var request = new InstallRequest(effectiveSettings, _services.Package);
        var updateAvailable = await Task.Run(() =>
            _services.Installation.IsPackageUpdateAvailable(_services.Package));
        var report = await Task.Run(() => _services.Installation.Verify(request));
        if (!report.IsValid && !updateAvailable)
        {
            throw new LauncherException(
                "Kontrola przed startem nie powiodła się:\n" + report.Summary);
        }

        var installText = updateAvailable
            ? "Wykryto nowszą lub inną paczkę. Poprzedni build zostanie bezpiecznie zastąpiony."
            : report.IsInstalled
                ? "Bieżący build jest poprawnie zainstalowany."
                : "Bieżący build zostanie teraz bezpiecznie zainstalowany.";
        var modeInstruction = _selectedMode == LauncherMode.Solo
            ? "Po wczytaniu Story Mode otwórz F9 i uruchom test solo."
            : _selectedMode == LauncherMode.Host
                ? $"W Story Mode wczytaj slot odpowiadający {Path.GetFileName(settings.HostSavePath)}."
                : "Po wejściu do Story Mode poczekaj na połączenie z hostem.";
        if (MessageBox.Show(
                this,
                $"{installText}\n\nUruchomić {modeName} przez {platformName}? " +
                $"{modeInstruction}\n\nWyłącznie Story Mode — nie otwieraj Red Dead Online.",
                $"Start {modeName}",
                MessageBoxButtons.YesNo,
                MessageBoxIcon.Warning) != DialogResult.Yes)
        {
            return;
        }

        if (updateAvailable)
        {
            await Task.Run(() => _services.Installation.UpdateToPackage(request));
            _services.Logger.Info(
                "quick_start.updated",
                "Launcher zastąpił poprzedni build bieżącą paczką przed startem.");
        }
        else if (!report.IsInstalled)
        {
            await Task.Run(() => _services.Installation.Install(request));
            _services.Logger.Info(
                "quick_start.installed",
                "Launcher zainstalował bieżący build przed startem.");
        }

        if (_selectedMode == LauncherMode.Solo)
        {
            await _services.Sidecar.StartSoloTestAsync(
                effectiveSettings,
                _services.Package,
                launchTarget);
            SetStatus(
                "Test solo uruchomiony. W Story Mode użyj F9 → „Test solo: start / stop”.",
                StatusKind.Success);
        }
        else
        {
            await _services.Sidecar.StartStoryModeAsync(
                effectiveSettings,
                _services.Package,
                launchTarget);
            SetStatus(
                $"{modeName} startuje przez {platformName}. Po teście wyeksportuj diagnostykę.",
                StatusKind.Success);
        }
    }

    private async Task VerifyAsync()
    {
        SaveSettings();
        var request = new InstallRequest(ReadSettings(), _services.Package);
        var updateAvailable = await Task.Run(() =>
            _services.Installation.IsPackageUpdateAvailable(_services.Package));
        var report = await Task.Run(() => _services.Installation.Verify(request));
        _installationState.Text = updateAvailable
            ? "↻ Dostępny jest inny build paczki — START może go zastąpić automatycznie."
            : report.IsValid
            ? report.IsInstalled
                ? "✓ Bieżący build jest poprawnie zainstalowany."
                : "○ Ścieżki są poprawne. Build nie jest jeszcze zainstalowany."
            : "! " + report.Summary.Replace(Environment.NewLine, "  •  ");
        _installationState.ForeColor = updateAvailable
            ? LauncherTheme.Warning
            : report.IsValid
            ? report.IsInstalled
                ? LauncherTheme.Success
                : LauncherTheme.Warning
            : LauncherTheme.Failure;
        SetStatus(
            updateAvailable
                ? "Wykryto zmianę paczki. Użyj Zainstaluj albo wróć i kliknij START."
                : report.Summary,
            updateAvailable || report.IsValid
                ? StatusKind.Success
                : StatusKind.Error);
    }

    private async Task InstallAsync()
    {
        SaveSettings();
        var request = new InstallRequest(ReadSettings(), _services.Package);
        var updateAvailable = await Task.Run(() =>
            _services.Installation.IsPackageUpdateAvailable(_services.Package));
        var report = await Task.Run(() => _services.Installation.Verify(request));
        if (!report.IsValid && !updateAvailable)
        {
            throw new LauncherException(report.Summary);
        }

        if (report.IsInstalled && !updateAvailable)
        {
            SetStatus("Bieżący build jest już zainstalowany.", StatusKind.Success);
            UpdateInstallationHint();
            return;
        }

        if (MessageBox.Show(
                this,
                (updateAvailable
                    ? "Launcher bezpiecznie zastąpi poprzedni build bieżącą paczką. "
                    : "Launcher zainstaluje bieżący build. ") +
                "Skopiuje ScriptHookRDR2.dll " +
                "i dinput8.dll ze wskazanego folderu. NativeTrainer.asi nie zostanie skopiowany. " +
                "Kontynuować?",
                "Bezpieczna instalacja",
                MessageBoxButtons.YesNo,
                MessageBoxIcon.Warning) != DialogResult.Yes)
        {
            return;
        }

        if (updateAvailable)
        {
            await Task.Run(() => _services.Installation.UpdateToPackage(request));
        }
        else
        {
            await Task.Run(() => _services.Installation.Install(request));
        }

        SetStatus(
            updateAvailable
                ? "Poprzedni build został zastąpiony bieżącą paczką."
                : "Bieżący build został zainstalowany.",
            StatusKind.Success);
        UpdateInstallationHint();
    }

    private async Task UninstallAsync()
    {
        if (_services.Sidecar.IsRunning)
        {
            throw new LauncherException(
                "Najpierw zatrzymaj sesję i zamknij RDR2.");
        }

        if (MessageBox.Show(
                this,
                "Launcher usunie wyłącznie pliki zapisane w manifeście jako własność " +
                "tej instalacji. Zamknij RDR2 i kontynuuj.",
                "Bezpieczna deinstalacja",
                MessageBoxButtons.YesNo,
                MessageBoxIcon.Warning) != DialogResult.Yes)
        {
            return;
        }

        await Task.Run(_services.Installation.Uninstall);
        SetStatus(
            "Mod odinstalowany. Można teraz bezpiecznie uruchomić Red Dead Online.",
            StatusKind.Success);
        UpdateInstallationHint();
    }

    private async Task ExportDiagnosticsAsync()
    {
        SaveSettings();
        var folder = ResolveDiagnosticsFolder();
        var destination = Path.Combine(folder, DiagnosticsFileName);
        var exported = await Task.Run(() => _services.Diagnostics.Export(
            destination,
            ReadSettings(),
            _services.Package));
        SetStatus(
            $"Diagnostyka gotowa: {exported}. Poprzedni ZIP został zastąpiony.",
            StatusKind.Success);
        UpdateDiagnosticsLocation();
    }

    private void StopSession()
    {
        _services.Sidecar.Stop();
        SetStatus(
            "Sidecar wyłączony. Wybierz HOSTUJ albo DOŁĄCZ, aby uruchomić nową sesję.",
            StatusKind.Warning);
        UpdateRunningUi();
    }

    private void DetectHostAddress(object? sender, EventArgs eventArgs)
    {
        SafeUiAction(() =>
        {
            var suggested = InviteService.SuggestedLanAddress();
            if (string.IsNullOrWhiteSpace(suggested))
            {
                throw new LauncherException(
                    "Nie wykryto aktywnego IPv4 Hamachi ani prywatnego LAN.");
            }

            _hostAddress.Text = suggested;
            SetStatus(
                suggested.StartsWith("25.", StringComparison.Ordinal)
                    ? $"Wykryto Hamachi: {suggested}."
                    : $"Wykryto LAN: {suggested}.",
                StatusKind.Success);
        });
    }

    private void BrowseGame(object? sender, EventArgs eventArgs)
    {
        using var dialog = new OpenFileDialog
        {
            Title = "Wskaż RDR2.exe",
            Filter = "RDR2.exe|RDR2.exe",
            CheckFileExists = true,
            Multiselect = false,
            InitialDirectory = GetExistingDirectory(_gamePath.Text)
        };
        if (dialog.ShowDialog(this) == DialogResult.OK)
        {
            _gamePath.Text = dialog.FileName;
            SaveSettingsSilently();
        }
    }

    private void BrowseRuntime(object? sender, EventArgs eventArgs)
    {
        using var dialog = new FolderBrowserDialog
        {
            Description = "Wskaż rozpakowany folder ScriptHookRDR2 albo jego folder bin",
            UseDescriptionForTitle = true,
            ShowNewFolderButton = false,
            InitialDirectory = Directory.Exists(_runtimePath.Text)
                ? _runtimePath.Text
                : string.Empty
        };
        if (dialog.ShowDialog(this) == DialogResult.OK)
        {
            _runtimePath.Text = dialog.SelectedPath;
            SaveSettingsSilently();
        }
    }

    private void BrowseDiagnosticsFolder(object? sender, EventArgs eventArgs)
    {
        using var dialog = new FolderBrowserDialog
        {
            Description = "Wybierz folder dla pliku RDR2-Coop-Diagnostics.zip",
            UseDescriptionForTitle = true,
            ShowNewFolderButton = true,
            InitialDirectory = Directory.Exists(_diagnosticsFolder.Text)
                ? _diagnosticsFolder.Text
                : ResolveDiagnosticsFolder()
        };
        if (dialog.ShowDialog(this) == DialogResult.OK)
        {
            _diagnosticsFolder.Text = dialog.SelectedPath;
            SaveSettingsSilently();
        }
    }

    private void BrowseHostSave(object? sender, EventArgs eventArgs)
    {
        SafeUiAction(() =>
        {
            var documents = Environment.GetFolderPath(
                Environment.SpecialFolder.MyDocuments);
            var profileRoot = Path.Combine(
                documents,
                "Rockstar Games",
                "Red Dead Redemption 2",
                "Profiles");
            using var dialog = new OpenFileDialog
            {
                Title = "Wybierz lokalny save hosta (SRDR*)",
                Filter = "Save RDR2 (SRDR*)|SRDR*|Wszystkie pliki (*.*)|*.*",
                CheckFileExists = true,
                Multiselect = false,
                InitialDirectory = Directory.Exists(profileRoot)
                    ? profileRoot
                    : documents
            };
            if (dialog.ShowDialog(this) != DialogResult.OK)
            {
                return;
            }

            if (!Path.GetFileName(dialog.FileName).StartsWith(
                    "SRDR",
                    StringComparison.OrdinalIgnoreCase))
            {
                throw new LauncherException("Wybierz plik save zaczynający się od SRDR.");
            }

            _hostSave.Text = Path.GetFullPath(dialog.FileName);
            SaveSettingsSilently();
            SetStatus(
                "Wybrano lokalny save hosta. Launcher go nie zmieni ani nie skopiuje.",
                StatusKind.Success);
        });
    }

    private void DetectCommonPaths()
    {
        SafeUiAction(() =>
        {
            var changed = false;
            if (!File.Exists(_gamePath.Text))
            {
                var detectedGame = FindGameExecutable();
                if (detectedGame is not null)
                {
                    _gamePath.Text = detectedGame;
                    changed = true;
                }
            }

            if (!IsRuntimeFolderValid(_runtimePath.Text))
            {
                var detectedRuntime = FindScriptHookFolder();
                if (detectedRuntime is not null)
                {
                    _runtimePath.Text = detectedRuntime;
                    changed = true;
                }
            }

            SaveSettings();
            SetStatus(
                changed
                    ? "Uzupełniono wykryte ścieżki. Sprawdź je przed startem."
                    : "Nie znaleziono nowych ścieżek. Wskaż je przyciskiem Przeglądaj.",
                changed ? StatusKind.Success : StatusKind.Warning);
        });
    }

    private string? FindGameExecutable()
    {
        var candidates = new List<string>();
        var steamPath = Registry.GetValue(
            @"HKEY_CURRENT_USER\Software\Valve\Steam",
            "SteamPath",
            null) as string;
        if (!string.IsNullOrWhiteSpace(steamPath))
        {
            candidates.Add(Path.Combine(
                steamPath,
                "steamapps",
                "common",
                "Red Dead Redemption 2",
                "RDR2.exe"));
        }

        var programFiles = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles);
        var programFilesX86 = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86);
        candidates.Add(Path.Combine(
            programFilesX86,
            "Steam",
            "steamapps",
            "common",
            "Red Dead Redemption 2",
            "RDR2.exe"));
        candidates.Add(Path.Combine(
            programFiles,
            "Rockstar Games",
            "Red Dead Redemption 2",
            "RDR2.exe"));
        foreach (var drive in DriveInfo.GetDrives())
        {
            try
            {
                if (!drive.IsReady || drive.DriveType != DriveType.Fixed)
                {
                    continue;
                }

                foreach (var relative in new[]
                         {
                             @"SteamLibrary\steamapps\common\Red Dead Redemption 2\RDR2.exe",
                             @"Programs\Steam\steamapps\common\Red Dead Redemption 2\RDR2.exe",
                             @"Games\Steam\steamapps\common\Red Dead Redemption 2\RDR2.exe"
                         })
                {
                    candidates.Add(Path.Combine(drive.RootDirectory.FullName, relative));
                }
            }
            catch (IOException)
            {
                // An unavailable removable or encrypted volume is irrelevant.
            }
            catch (UnauthorizedAccessException)
            {
                // Continue with the other common library locations.
            }
        }

        return candidates.FirstOrDefault(File.Exists);
    }

    private string? FindScriptHookFolder()
    {
        var cursor = new DirectoryInfo(_services.Package.Root);
        for (var depth = 0; cursor is not null && depth < 7; depth++, cursor = cursor.Parent)
        {
            var candidate = Path.Combine(cursor.FullName, "ScriptHookRDR2_1.0.1491.17");
            if (IsRuntimeFolderValid(candidate))
            {
                return candidate;
            }
        }

        return null;
    }

    private void LoadSettings()
    {
        var hadSettings = File.Exists(_services.Paths.SettingsPath);
        try
        {
            var settings = _services.Settings.Load();
            _gamePath.Text = settings.GameExePath;
            _runtimePath.Text = settings.ScriptHookFolder;
            _nickname.Text = settings.Nickname;
            _hostAddress.Text = settings.HostAddress;
            _sessionToken = settings.SessionToken;
            _hostSave.Text = settings.HostSavePath;
            _animGraphReplica.Checked =
                settings.MotionReplicationMode ==
                LauncherMotionReplicationMode.AnimGraphReplica;
            _storyVmProbe.Checked =
                settings.AnimSceneStoryVmProbeEnabled;
            _diagnosticsFolder.Text = string.IsNullOrWhiteSpace(
                settings.DiagnosticsExportFolder)
                ? DefaultDiagnosticsFolder()
                : settings.DiagnosticsExportFolder;

            if (hadSettings)
            {
                _selectedMode = settings.LastMode ??
                    (settings.Role == LauncherRole.Guest
                        ? LauncherMode.Guest
                        : LauncherMode.Host);
                _selectedPlatform = settings.Platform;
            }
        }
        catch (LauncherException exception)
        {
            _services.Logger.Error("settings.load_failed", exception);
            _diagnosticsFolder.Text = DefaultDiagnosticsFolder();
            SetStatus(exception.Message, StatusKind.Error);
        }

        if (string.IsNullOrWhiteSpace(_nickname.Text))
        {
            _nickname.Text = "Player";
        }
    }

    private LauncherSettings ReadSettings()
    {
        var fallbackRole = _selectedMode == LauncherMode.Guest
            ? LauncherRole.Guest
            : LauncherRole.Host;
        return new LauncherSettings
        {
            GameExePath = _gamePath.Text.Trim(),
            ScriptHookFolder = _runtimePath.Text.Trim(),
            DiagnosticsExportFolder = ResolveDiagnosticsFolder(),
            Nickname = _nickname.Text.Trim(),
            Role = fallbackRole,
            Platform = _selectedPlatform ?? LauncherPlatform.Steam,
            HostAddress = _hostAddress.Text.Trim(),
            SessionToken = _sessionToken.Trim(),
            HostSavePath = _hostSave.Text.Trim(),
            MotionReplicationMode = _animGraphReplica.Checked
                ? LauncherMotionReplicationMode.AnimGraphReplica
                : LauncherMotionReplicationMode.TaskNavmesh,
            AnimSceneStoryVmProbeEnabled = _storyVmProbe.Checked,
            LastMode = _selectedMode
        };
    }

    private void SaveSettings() => _services.Settings.Save(ReadSettings());

    private void SaveSettingsSilently()
    {
        if (_loading)
        {
            return;
        }

        try
        {
            SaveSettings();
        }
        catch (Exception exception)
        {
            _services.Logger.Error("settings.auto_save_failed", exception);
        }
    }

    private void SaveSettingsWithFeedback()
    {
        SafeUiAction(() =>
        {
            SaveSettings();
            SetStatus(
                "Ustawienia zapisane. Będą używane także przez kolejne paczki release.",
                StatusKind.Success);
            UpdateReadiness();
        });
    }

    private async Task RunActionAsync(Func<Task> action)
    {
        if (_busy)
        {
            return;
        }

        _busy = true;
        UpdateReadiness();
        try
        {
            await action();
        }
        catch (Exception exception)
        {
            _services.Logger.Error("ui.action_failed", exception);
            SetStatus(exception.Message, StatusKind.Error);
            MessageBox.Show(
                this,
                $"{exception.Message}\n\nSzczegóły zapisano w:\n{_services.Paths.LogDirectory}",
                "Operacja zatrzymana",
                MessageBoxButtons.OK,
                MessageBoxIcon.Error);
        }
        finally
        {
            _busy = false;
            UpdateRunningUi();
        }
    }

    private void SafeUiAction(Action action)
    {
        try
        {
            action();
        }
        catch (Exception exception)
        {
            _services.Logger.Error("ui.action_failed", exception);
            SetStatus(exception.Message, StatusKind.Error);
            MessageBox.Show(
                this,
                $"{exception.Message}\n\nSzczegóły zapisano w:\n{_services.Paths.LogDirectory}",
                "Operacja zatrzymana",
                MessageBoxButtons.OK,
                MessageBoxIcon.Error);
        }
    }

    private bool PromptHostPassword(out string? password)
    {
        password = null;
        using var dialog = new Form
        {
            Text = "Ustaw hasło sesji",
            StartPosition = FormStartPosition.CenterParent,
            FormBorderStyle = FormBorderStyle.FixedDialog,
            MaximizeBox = false,
            MinimizeBox = false,
            ShowInTaskbar = false,
            ClientSize = new Size(470, 330),
            BackColor = LauncherTheme.Background,
            ForeColor = LauncherTheme.Text,
            Font = Font
        };
        var layout = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 1,
            RowCount = 8,
            Padding = new Padding(24, 20, 24, 18),
            BackColor = LauncherTheme.Background
        };
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 48));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 22));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 42));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 22));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 42));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 38));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 32));
        layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        dialog.Controls.Add(layout);

        var heading = new Label
        {
            Dock = DockStyle.Fill,
            Text = "HOSTUJ SESJĘ  •  USTAW HASŁO",
            Font = CreateDisplayFont(15f),
            ForeColor = LauncherTheme.Host,
            TextAlign = ContentAlignment.MiddleLeft
        };
        layout.Controls.Add(heading, 0, 0);
        layout.Controls.Add(MakeFieldLabel("HASŁO SESJI"), 0, 1);
        var entered = CreateTextBox();
        entered.MaxLength = SessionPasswordService.MaximumLength;
        entered.UseSystemPasswordChar = true;
        entered.PlaceholderText = "minimum 4 znaki";
        layout.Controls.Add(entered, 0, 2);
        layout.Controls.Add(MakeFieldLabel("POWTÓRZ HASŁO"), 0, 3);
        var confirmation = CreateTextBox();
        confirmation.MaxLength = SessionPasswordService.MaximumLength;
        confirmation.UseSystemPasswordChar = true;
        layout.Controls.Add(confirmation, 0, 4);
        var help = new Label
        {
            Dock = DockStyle.Fill,
            ForeColor = LauncherTheme.TextDim,
            Text = "Przekaż znajomemu IPv4 oraz to samo hasło. Hasło nie jest zapisywane jawnie.",
            TextAlign = ContentAlignment.MiddleLeft
        };
        layout.Controls.Add(help, 0, 5);
        var validation = new Label
        {
            Dock = DockStyle.Fill,
            ForeColor = LauncherTheme.Failure,
            TextAlign = ContentAlignment.MiddleLeft
        };
        layout.Controls.Add(validation, 0, 6);

        var buttons = new FlowLayoutPanel
        {
            Dock = DockStyle.Fill,
            FlowDirection = FlowDirection.RightToLeft,
            WrapContents = false,
            BackColor = Color.Transparent
        };
        var save = new Button
        {
            Text = "ZAPISZ I HOSTUJ",
            Width = 160,
            Height = 38,
            BackColor = LauncherTheme.Host,
            ForeColor = Color.White,
            FlatStyle = FlatStyle.Flat
        };
        save.FlatAppearance.BorderSize = 0;
        var cancel = new Button
        {
            Text = "ANULUJ",
            Width = 105,
            Height = 38,
            BackColor = LauncherTheme.SurfaceRaised,
            ForeColor = LauncherTheme.Text,
            FlatStyle = FlatStyle.Flat,
            DialogResult = DialogResult.Cancel
        };
        cancel.FlatAppearance.BorderColor = LauncherTheme.Border;
        save.Click += (_, _) =>
        {
            try
            {
                SessionPasswordService.Validate(entered.Text);
                if (!string.Equals(
                        entered.Text,
                        confirmation.Text,
                        StringComparison.Ordinal))
                {
                    throw new LauncherException("Powtórzone hasło nie jest identyczne.");
                }

                dialog.DialogResult = DialogResult.OK;
                dialog.Close();
            }
            catch (LauncherException exception)
            {
                validation.Text = exception.Message;
            }
        };
        buttons.Controls.Add(save);
        buttons.Controls.Add(cancel);
        layout.Controls.Add(buttons, 0, 7);
        dialog.AcceptButton = save;
        dialog.CancelButton = cancel;
        dialog.Shown += (_, _) => entered.Focus();

        if (dialog.ShowDialog(this) != DialogResult.OK)
        {
            return false;
        }

        password = entered.Text;
        entered.Clear();
        confirmation.Clear();
        return true;
    }

    private bool PromptGuestConnection(out string? password)
    {
        password = null;
        using var dialog = new Form
        {
            Text = "Dołącz do lobby",
            StartPosition = FormStartPosition.CenterParent,
            FormBorderStyle = FormBorderStyle.FixedDialog,
            MaximizeBox = false,
            MinimizeBox = false,
            ShowInTaskbar = false,
            ClientSize = new Size(470, 285),
            BackColor = LauncherTheme.Background,
            ForeColor = LauncherTheme.Text,
            Font = Font
        };
        var layout = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 1,
            RowCount = 7,
            Padding = new Padding(24, 20, 24, 18),
            BackColor = LauncherTheme.Background
        };
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 48));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 22));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 42));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 22));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 42));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 32));
        layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        dialog.Controls.Add(layout);

        var heading = new Label
        {
            Dock = DockStyle.Fill,
            Text = "DOŁĄCZ DO SESJI  •  GUEST",
            Font = CreateDisplayFont(15f),
            ForeColor = LauncherTheme.Guest,
            TextAlign = ContentAlignment.MiddleLeft
        };
        layout.Controls.Add(heading, 0, 0);
        layout.Controls.Add(MakeFieldLabel("IPV4 HOSTA — HAMACHI / LAN"), 0, 1);
        var address = CreateTextBox();
        address.Text = _hostAddress.Text.Trim();
        address.PlaceholderText = "np. adres Hamachi 25.x.x.x";
        layout.Controls.Add(address, 0, 2);
        layout.Controls.Add(MakeFieldLabel("HASŁO SESJI HOSTA"), 0, 3);
        var entered = CreateTextBox();
        entered.MaxLength = SessionPasswordService.MaximumLength;
        entered.PlaceholderText = "hasło ustawione przez hosta";
        entered.UseSystemPasswordChar = true;
        layout.Controls.Add(entered, 0, 4);
        var validation = new Label
        {
            Dock = DockStyle.Fill,
            ForeColor = LauncherTheme.Failure,
            TextAlign = ContentAlignment.MiddleLeft
        };
        layout.Controls.Add(validation, 0, 5);
        var buttons = new FlowLayoutPanel
        {
            Dock = DockStyle.Fill,
            FlowDirection = FlowDirection.RightToLeft,
            WrapContents = false,
            BackColor = Color.Transparent
        };
        var join = new Button
        {
            Text = "DOŁĄCZ",
            Width = 125,
            Height = 38,
            BackColor = LauncherTheme.Guest,
            ForeColor = Color.White,
            FlatStyle = FlatStyle.Flat
        };
        join.FlatAppearance.BorderSize = 0;
        var cancel = new Button
        {
            Text = "ANULUJ",
            Width = 105,
            Height = 38,
            BackColor = LauncherTheme.SurfaceRaised,
            ForeColor = LauncherTheme.Text,
            FlatStyle = FlatStyle.Flat,
            DialogResult = DialogResult.Cancel
        };
        cancel.FlatAppearance.BorderColor = LauncherTheme.Border;
        join.Click += (_, _) =>
        {
            try
            {
                if (!IPAddress.TryParse(address.Text.Trim(), out var parsed) ||
                    parsed.AddressFamily != AddressFamily.InterNetwork)
                {
                    throw new LauncherException(
                        "Wpisz poprawny IPv4 hosta, np. adres Hamachi 25.x.x.x.");
                }
                _ = InviteService.ValidateRemoteHost(address.Text.Trim());
                SessionPasswordService.Validate(entered.Text);
                dialog.DialogResult = DialogResult.OK;
                dialog.Close();
            }
            catch (LauncherException exception)
            {
                validation.Text = exception.Message;
            }
            catch (FormatException exception)
            {
                validation.Text = exception.Message;
            }
        };
        buttons.Controls.Add(join);
        buttons.Controls.Add(cancel);
        layout.Controls.Add(buttons, 0, 6);
        dialog.AcceptButton = join;
        dialog.CancelButton = cancel;
        dialog.Shown += (_, _) => address.Focus();

        if (dialog.ShowDialog(this) != DialogResult.OK)
        {
            return false;
        }

        _hostAddress.Text = address.Text.Trim();
        password = entered.Text;
        entered.Clear();
        UpdateReadiness();
        UpdateLobbyPreview();
        return true;
    }

    private void UpdateLobbyPreview()
    {
        if (_services.Sidecar.IsRunning)
        {
            _lobby.Snapshot = _services.Sidecar.Lobby;
            return;
        }

        var localRole = _selectedMode == LauncherMode.Guest
            ? LauncherRole.Guest
            : LauncherRole.Host;
        _lobby.Snapshot = new LauncherLobbySnapshot(
            string.IsNullOrWhiteSpace(_nickname.Text)
                ? "Player"
                : _nickname.Text.Trim(),
            localRole,
            _selectedMode == LauncherMode.Solo ? "SOLO BOT" : string.Empty,
            localRole == LauncherRole.Host
                ? LauncherRole.Guest
                : LauncherRole.Host,
            localRole == LauncherRole.Guest
                ? _hostAddress.Text.Trim()
                : string.Empty,
            false,
            false,
            false,
            null);
    }

    private static void WarmControlTree(Control root)
    {
        _ = root.Handle;
        foreach (Control child in root.Controls)
        {
            WarmControlTree(child);
        }
    }

    private void ShowPage(bool settings)
    {
        _showingSettings = settings;
        var target = settings ? _settingsPage : _homePage;
        var previous = settings ? _homePage : _settingsPage;
        _pageHost.SuspendLayout();
        try
        {
            target.Visible = true;
            target.BringToFront();
            if (_warmedPages.Add(target))
            {
                WarmControlTree(target);
            }
            target.PerformLayout();
            previous.Visible = false;
        }
        finally
        {
            _pageHost.ResumeLayout(performLayout: true);
        }
        _pageHost.Invalidate(invalidateChildren: true);
        _pageHost.Update();

        _homeNav.Accent = !settings;
        _settingsNav.Accent = settings;
        _homeNav.Invalidate();
        _settingsNav.Invalidate();
        if (settings)
        {
            UpdateInstallationHint();
            UpdateDiagnosticsLocation();
        }
    }

    private void UpdateRunningUi()
    {
        var running = _services.Sidecar.IsRunning;
        _sidecarState.Text = running ? "●  SESJA AKTYWNA" : "○  SESJA ZATRZYMANA";
        _sidecarState.ForeColor = running
            ? LauncherTheme.Success
            : LauncherTheme.TextDim;
        UpdateReadiness();
        if (!running)
        {
            UpdateLobbyPreview();
        }
    }

    private void UpdateInstallationHint()
    {
        var manifestPresent = File.Exists(_services.Paths.InstallManifestPath);
        _installationState.Text = manifestPresent
            ? "○ Manifest istnieje — kliknij Sprawdź, aby potwierdzić zgodność builda."
            : "○ Build nie jest zainstalowany lub brak lokalnego manifestu.";
        _installationState.ForeColor = manifestPresent
            ? LauncherTheme.Warning
            : LauncherTheme.TextMuted;
    }

    private void UpdateDiagnosticsLocation()
    {
        if (_diagnosticsLocation.IsDisposed)
        {
            return;
        }

        string path;
        try
        {
            path = Path.Combine(ResolveDiagnosticsFolder(), DiagnosticsFileName);
        }
        catch (Exception exception) when (
            exception is ArgumentException or NotSupportedException or PathTooLongException)
        {
            _diagnosticsLocation.Text =
                "NIEPRAWIDŁOWA ŚCIEŻKA EKSPORTU\n" + exception.Message;
            _diagnosticsLocation.ForeColor = LauncherTheme.Failure;
            return;
        }

        _diagnosticsLocation.ForeColor = LauncherTheme.TextMuted;
        _diagnosticsLocation.Text =
            "EKSPORT JEDNYM KLIKNIĘCIEM\n" +
            path +
            "\nNowy eksport bezpiecznie zastępuje poprzedni plik ZIP.";
    }

    private void SetStatus(string text, StatusKind kind)
    {
        _status.Text = text.Replace(Environment.NewLine, "  •  ");
        var color = kind switch
        {
            StatusKind.Success => LauncherTheme.Success,
            StatusKind.Warning => LauncherTheme.Warning,
            StatusKind.Error => LauncherTheme.Failure,
            _ => LauncherTheme.TextMuted
        };
        _status.ForeColor = color;
        _statusDot.BackColor = color;
    }

    private void LoggerOnLineWritten(object? sender, string line)
    {
        if (IsDisposed)
        {
            return;
        }

        if (InvokeRequired)
        {
            BeginInvoke(() => LoggerOnLineWritten(sender, line));
            return;
        }

        _activityLog.AppendText($"{DateTime.Now:HH:mm:ss}  {line}{Environment.NewLine}");
        _activityLog.SelectionStart = _activityLog.TextLength;
        _activityLog.ScrollToCaret();
    }

    private void SidecarOnRunningChanged(object? sender, bool running)
    {
        if (!IsDisposed)
        {
            BeginInvoke(UpdateRunningUi);
        }
    }

    private void SidecarOnLobbyChanged(
        object? sender,
        LauncherLobbySnapshot snapshot)
    {
        if (IsDisposed)
        {
            return;
        }
        if (InvokeRequired)
        {
            BeginInvoke(() => SidecarOnLobbyChanged(sender, snapshot));
            return;
        }
        _lobby.Snapshot = snapshot;
    }

    private void OnFormClosing(object? sender, FormClosingEventArgs eventArgs)
    {
        if (_services.Sidecar.IsRunning &&
            MessageBox.Show(
                this,
                "Sidecar nadal działa. Zamknięcie launchera zatrzyma bieżący test. Zamknąć?",
                "Sesja jest uruchomiona",
                MessageBoxButtons.YesNo,
                MessageBoxIcon.Warning) != DialogResult.Yes)
        {
            eventArgs.Cancel = true;
            return;
        }

        SaveSettingsSilently();
        _services.Logger.LineWritten -= LoggerOnLineWritten;
        _services.Sidecar.RunningChanged -= SidecarOnRunningChanged;
        _services.Sidecar.LobbyChanged -= SidecarOnLobbyChanged;
        _services.Dispose();
    }

    private void OpenLogs()
    {
        _services.Paths.EnsureDirectories();
        OpenFolder(_services.Paths.LogDirectory);
    }

    private void OpenDiagnosticsFolder()
    {
        var folder = ResolveDiagnosticsFolder();
        Directory.CreateDirectory(folder);
        OpenFolder(folder);
    }

    private static void OpenFolder(string folder) =>
        _ = Process.Start(new ProcessStartInfo
        {
            FileName = folder,
            UseShellExecute = true
        });

    private string ResolveDiagnosticsFolder()
    {
        var value = _diagnosticsFolder.Text.Trim();
        return string.IsNullOrWhiteSpace(value)
            ? DefaultDiagnosticsFolder()
            : Path.GetFullPath(value);
    }

    private static string DefaultDiagnosticsFolder()
    {
        var documents = Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments);
        return Path.Combine(documents, "RDR2 Coop Story", "Diagnostyka");
    }

    private static Font CreateDisplayFont(float size) =>
        new("Georgia", size, FontStyle.Bold, GraphicsUnit.Point);

    private string ReadPackageDisplayName()
    {
        var buildInfoPath = Path.Combine(_services.Package.Root, "BUILD_INFO.json");
        if (File.Exists(buildInfoPath))
        {
            try
            {
                using var document = JsonDocument.Parse(File.ReadAllBytes(buildInfoPath));
                if (document.RootElement.TryGetProperty("package", out var packageElement) &&
                    packageElement.ValueKind == JsonValueKind.String)
                {
                    var package = packageElement.GetString() ?? string.Empty;
                    var prefix = "RDR2-CoopStory-";
                    if (package.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
                    {
                        package = package[prefix.Length..];
                    }

                    var timestampMarker = package.LastIndexOf("-20", StringComparison.Ordinal);
                    if (timestampMarker > 0)
                    {
                        package = package[..timestampMarker];
                    }

                    return AddWordSpaces(package).ToUpperInvariant();
                }
            }
            catch (JsonException)
            {
                // A package without valid metadata still has assembly version data.
            }
        }

        return $"DEV BUILD V{ReadLauncherVersion()}";
    }

    private static string ReadLauncherVersion()
    {
        var assembly = Assembly.GetExecutingAssembly();
        var informational = assembly
            .GetCustomAttribute<AssemblyInformationalVersionAttribute>()?
            .InformationalVersion;
        return string.IsNullOrWhiteSpace(informational)
            ? assembly.GetName().Version?.ToString(3) ?? "DEV"
            : informational.Split('+')[0];
    }

    private static string AddWordSpaces(string value)
    {
        var output = new System.Text.StringBuilder(value.Length + 8);
        for (var index = 0; index < value.Length; index++)
        {
            if (index > 0 &&
                char.IsUpper(value[index]) &&
                char.IsLower(value[index - 1]))
            {
                output.Append(' ');
            }

            output.Append(value[index] == '-' ? ' ' : value[index]);
        }

        return output.ToString();
    }

    private static bool IsNicknameValid(string nickname)
    {
        try
        {
            _ = PlayerIdentityRules.ValidateNickname(nickname.Trim());
            return true;
        }
        catch (ArgumentException)
        {
            return false;
        }
    }

    private static bool IsRuntimeFolderValid(string folder)
    {
        try
        {
            _ = PackageLocator.LocateRuntime(folder);
            return true;
        }
        catch (Exception exception) when (
            exception is LauncherException or ArgumentException or IOException or UnauthorizedAccessException)
        {
            return false;
        }
    }

    private static bool IsHostAddressValid(string address)
    {
        try
        {
            if (!IPAddress.TryParse(address.Trim(), out var parsed) ||
                parsed.AddressFamily != AddressFamily.InterNetwork)
            {
                return false;
            }
            _ = InviteService.ValidateRemoteHost(address);
            return true;
        }
        catch (LauncherException)
        {
            return false;
        }
    }

    private static bool IsSavePathValid(string path) =>
        File.Exists(path) &&
        Path.GetFileName(path).StartsWith("SRDR", StringComparison.OrdinalIgnoreCase);

    private static string ReadyMark(bool ready) => ready ? "✓" : "○";

    private static string GetExistingDirectory(string path)
    {
        try
        {
            var directory = Path.GetDirectoryName(path);
            return Directory.Exists(directory) ? directory : string.Empty;
        }
        catch (ArgumentException)
        {
            return string.Empty;
        }
    }

    private static TextBox CreateTextBox() =>
        new()
        {
            Dock = DockStyle.Fill,
            BorderStyle = BorderStyle.FixedSingle,
            BackColor = LauncherTheme.SurfaceRaised,
            ForeColor = LauncherTheme.Text,
            Font = new Font("Segoe UI", 10f),
            Margin = Padding.Empty
        };

    private static Label MakeFieldLabel(string text) =>
        new()
        {
            Dock = DockStyle.Fill,
            Text = text,
            Font = new Font("Segoe UI", 8f, FontStyle.Bold),
            ForeColor = LauncherTheme.TextDim,
            TextAlign = ContentAlignment.BottomLeft,
            Padding = new Padding(1, 0, 0, 3),
            Margin = Padding.Empty
        };

    private Control CreateFieldBlock(
        string label,
        TextBox textBox,
        RdrActionButton action)
    {
        var block = new TableLayoutPanel
        {
            Dock = DockStyle.Top,
            Height = 76,
            ColumnCount = 2,
            RowCount = 2,
            BackColor = Color.Transparent,
            Margin = Padding.Empty,
            Padding = new Padding(0, 0, 0, 6)
        };
        block.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        block.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, action.Width + 8));
        block.RowStyles.Add(new RowStyle(SizeType.Absolute, 26));
        block.RowStyles.Add(new RowStyle(SizeType.Absolute, 42));
        block.Controls.Add(MakeFieldLabel(label), 0, 0);
        block.SetColumnSpan(block.GetControlFromPosition(0, 0)!, 2);
        textBox.Dock = DockStyle.Fill;
        textBox.Margin = new Padding(0, 0, 8, 0);
        action.Dock = DockStyle.Fill;
        action.Margin = Padding.Empty;
        block.Controls.Add(textBox, 0, 1);
        block.Controls.Add(action, 1, 1);
        return block;
    }

    private Control CreateSectionHeading(
        string number,
        string title,
        string description)
    {
        var panel = new Panel
        {
            Dock = DockStyle.Fill,
            BackColor = Color.Transparent,
            Margin = Padding.Empty
        };
        var titleLeft = string.IsNullOrWhiteSpace(number) ? 0 : 44;
        if (!string.IsNullOrWhiteSpace(number))
        {
            var step = new Label
            {
                AutoSize = false,
                Location = new Point(0, 3),
                Size = new Size(34, 34),
                BackColor = LauncherTheme.Bar,
                ForeColor = LauncherTheme.Text,
                Font = new Font(Font.FontFamily, 8.5f, FontStyle.Bold),
                Text = number,
                TextAlign = ContentAlignment.MiddleCenter
            };
            panel.Controls.Add(step);
        }

        var heading = new Label
        {
            AutoSize = true,
            Location = new Point(titleLeft, 0),
            Font = CreateDisplayFont(16f),
            ForeColor = LauncherTheme.Text,
            Text = title
        };
        var subtitle = new Label
        {
            AutoSize = false,
            Location = new Point(titleLeft, 34),
            Size = new Size(Math.Max(100, 500 - titleLeft), 38),
            Anchor = AnchorStyles.Left | AnchorStyles.Right | AnchorStyles.Top,
            Font = new Font(Font.FontFamily, 8.8f),
            ForeColor = LauncherTheme.TextMuted,
            Text = description
        };
        panel.Controls.Add(subtitle);
        panel.Controls.Add(heading);
        return panel;
    }

    private static RdrSurface CreateSurface(Padding padding) =>
        new()
        {
            Dock = DockStyle.Fill,
            Padding = padding,
            FillColor = LauncherTheme.Surface,
            OutlineColor = LauncherTheme.Border,
            CornerRadius = 18
        };

    private RdrActionButton MakeButton(
        string text,
        RdrIcon icon,
        EventHandler handler)
    {
        var button = new RdrActionButton
        {
            Text = text,
            Icon = icon,
            Font = new Font(Font.FontFamily, 8.5f),
            Width = 140,
            Height = 38,
            Margin = new Padding(4, 0, 4, 0)
        };
        button.Click += handler;
        _actionButtons.Add(button);
        return button;
    }

    private void TryEnableDarkTitleBar()
    {
        if (!OperatingSystem.IsWindows())
        {
            return;
        }

        var enabled = 1;
        if (DwmSetWindowAttribute(Handle, 20, ref enabled, sizeof(int)) != 0)
        {
            _ = DwmSetWindowAttribute(Handle, 19, ref enabled, sizeof(int));
        }
    }

    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(
        IntPtr window,
        int attribute,
        ref int value,
        int valueSize);

    private enum StatusKind
    {
        Neutral,
        Success,
        Warning,
        Error
    }
}
