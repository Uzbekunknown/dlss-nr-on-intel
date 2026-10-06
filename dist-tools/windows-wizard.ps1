param([string]$Root = '', [switch]$SelfTest, [string]$ControlSchema = '')
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName PresentationFramework, PresentationCore, WindowsBase, System.Windows.Forms
if (-not $Root) { $Root = [IO.Path]::GetDirectoryName($PSCommandPath) }
$script:Root = [IO.Path]::GetFullPath($Root)
$script:Bridge = Join-Path $script:Root 'scripts\windows_wizard.py'
$script:Work = Join-Path $script:Root 'work\windows-wizard'
$script:Busy = $null
$script:StatusJob = $null
$script:TickCount = 0
$script:Ready = $false
$script:Installed = $false
$script:LastProfile = $null
$script:BootPython = ''
$script:KnobUi = @{}
$script:PendingSettings = @{}
$script:LoadingSettings = $false
$script:SettingsReload = $false
$script:SettingsDue = [DateTime]::MaxValue
$script:SettingsStateKey = 'SettingsLoading'

[xml]$xaml = @'
<Window xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
 Title="DLSS-NR" Width="940" Height="900" MinWidth="820" MinHeight="690" WindowStartupLocation="CenterScreen"
 FontFamily="Segoe UI" FontSize="14" Background="#F3F6FA">
 <Window.Resources>
  <Style TargetType="Button"><Setter Property="Padding" Value="14,9"/><Setter Property="Margin" Value="0,0,8,0"/><Setter Property="Background" Value="White"/><Setter Property="BorderBrush" Value="#CDD6E3"/><Setter Property="Cursor" Value="Hand"/></Style>
  <Style TargetType="TextBox"><Setter Property="Padding" Value="9,7"/><Setter Property="BorderBrush" Value="#CDD6E3"/><Setter Property="VerticalContentAlignment" Value="Center"/></Style>
  <Style TargetType="ComboBox"><Setter Property="Padding" Value="7"/><Setter Property="VerticalContentAlignment" Value="Center"/></Style>
 </Window.Resources>
 <DockPanel Margin="28">
  <DockPanel DockPanel.Dock="Top" Margin="0,0,0,20">
   <StackPanel DockPanel.Dock="Right" Orientation="Horizontal" VerticalAlignment="Top">
    <TextBlock x:Name="LanguageLabel" VerticalAlignment="Center" Margin="0,0,10,0"/>
    <ComboBox x:Name="Language" Width="125" ToolTip="">
     <ComboBoxItem Content="English" Tag="en"/>
     <ComboBoxItem Content="Русский" Tag="ru"/>
    </ComboBox>
   </StackPanel>
   <StackPanel>
    <TextBlock Text="DLSS-NR" FontSize="30" FontWeight="SemiBold" Foreground="#14243B"/>
    <TextBlock x:Name="Subtitle" Text="" Foreground="#53647A" Margin="0,5,0,0" TextWrapping="Wrap"/>
   </StackPanel>
  </DockPanel>
  <TabControl x:Name="Pages" Background="Transparent" BorderThickness="0">
   <TabItem x:Name="SetupTab" Header="">
  <ScrollViewer x:Name="MainScroll" VerticalScrollBarVisibility="Auto">
   <StackPanel>
    <Border Background="White" CornerRadius="10" Padding="20" Margin="0,0,0,14">
     <StackPanel>
      <TextBlock x:Name="FilesHeading" Text="" FontSize="19" FontWeight="SemiBold" Margin="0,0,0,14"/>
      <Grid><Grid.ColumnDefinitions><ColumnDefinition Width="155"/><ColumnDefinition Width="*"/><ColumnDefinition Width="105"/></Grid.ColumnDefinitions>
       <Grid.RowDefinitions><RowDefinition Height="Auto"/><RowDefinition Height="Auto"/><RowDefinition Height="Auto"/></Grid.RowDefinitions>
       <TextBlock x:Name="DllLabel" Text="" VerticalAlignment="Center"/><TextBox x:Name="DllPath" Grid.Column="1" Margin="0,0,10,10" ToolTip=""/><Button x:Name="BrowseDll" Grid.Column="2" Content="" Margin="0,0,0,10"/>
       <TextBlock x:Name="GameLabel" Grid.Row="1" Text="" VerticalAlignment="Center"/><TextBox x:Name="GamePath" Grid.Row="1" Grid.Column="1" Margin="0,0,10,10" ToolTip=""/><Button x:Name="BrowseGame" Grid.Row="1" Grid.Column="2" Content="" Margin="0,0,0,10"/>
       <TextBlock x:Name="PythonLabel" Grid.Row="2" Text="" VerticalAlignment="Center"/><TextBox x:Name="PythonPath" Grid.Row="2" Grid.Column="1" Margin="0,0,10,0"/><Button x:Name="BrowsePython" Grid.Row="2" Grid.Column="2" Content="" Margin="0"/>
      </Grid>
      <WrapPanel Margin="155,10,0,0"><Button x:Name="FindPython" Content=""/><Button x:Name="GetPython" Content=""/></WrapPanel>
      <TextBlock x:Name="PythonHint" Text="" TextWrapping="Wrap" Foreground="#64748B" Margin="0,12,0,0" FontSize="12"/>
     </StackPanel>
    </Border>
    <Border Background="White" CornerRadius="10" Padding="20" Margin="0,0,0,14">
     <StackPanel>
      <TextBlock x:Name="ConnectHeading" Text="" FontSize="19" FontWeight="SemiBold" Margin="0,0,0,14"/>
      <Grid><Grid.ColumnDefinitions><ColumnDefinition Width="155"/><ColumnDefinition Width="*"/><ColumnDefinition Width="110"/><ColumnDefinition Width="*"/></Grid.ColumnDefinitions>
       <TextBlock x:Name="ApiLabel" Text="" VerticalAlignment="Center"/><ComboBox x:Name="Api" Grid.Column="1" SelectedIndex="0" Margin="0,0,16,0"><ComboBoxItem x:Name="ApiVulkan" Content=""/><ComboBoxItem x:Name="ApiDxvk" Content=""/></ComboBox>
       <TextBlock x:Name="ModeLabel" Text="" Grid.Column="2" VerticalAlignment="Center"/><ComboBox x:Name="Mode" Grid.Column="3" SelectedIndex="0"><ComboBoxItem x:Name="ModeDirect" Content=""/><ComboBoxItem x:Name="ModeSteam" Content=""/></ComboBox>
      </Grid>
      <Expander x:Name="LaunchOptions" Header="" Margin="0,14,0,0">
       <StackPanel Margin="0,10,0,0">
        <TextBlock x:Name="ArgsLabel" Text="" Margin="0,0,0,5"/><TextBox x:Name="GameArgs" ToolTip=""/>
        <Grid Margin="0,10,0,0"><Grid.ColumnDefinitions><ColumnDefinition Width="155"/><ColumnDefinition Width="*"/></Grid.ColumnDefinitions><TextBlock x:Name="SteamIdLabel" Text="" VerticalAlignment="Center"/><TextBox x:Name="SteamId" Grid.Column="1" ToolTip=""/></Grid>
        <CheckBox x:Name="Fossilize" IsChecked="True" Content="" Margin="0,10,0,0" ToolTip=""/>
       </StackPanel>
      </Expander>
      <TextBlock x:Name="FirstTestHint" Text="" TextWrapping="Wrap" Foreground="#64748B" FontSize="12" Margin="0,14,0,14"/>
      <WrapPanel><Button x:Name="Check" Content=""/><Button x:Name="Dependencies" Content=""/><Button x:Name="Install" Content="" Background="#1D4ED8" Foreground="White" BorderBrush="#1D4ED8"/></WrapPanel>
     </StackPanel>
    </Border>
    <Border Background="White" CornerRadius="10" Padding="20" Margin="0,0,0,14">
     <StackPanel>
      <TextBlock x:Name="CompareHeading" Text="" FontSize="19" FontWeight="SemiBold" Margin="0,0,0,12"/>
      <WrapPanel><Button x:Name="Launch" Content=""/><Button x:Name="Toggle" Content=""/><Button x:Name="SteamSetup" Content=""/><Button x:Name="SteamRestore" Content=""/></WrapPanel>
      <TextBlock x:Name="EffectState" Text="" FontWeight="SemiBold" Foreground="#1D4ED8" Margin="0,14,0,4"/>
      <TextBlock x:Name="RuntimeState" Text="" TextWrapping="Wrap" Foreground="#53647A"/>
      <TextBlock x:Name="FpsHint" Text="" Foreground="#64748B" TextWrapping="Wrap" FontSize="12" Margin="0,10,0,0"/>
     </StackPanel>
    </Border>
    <Border Background="White" CornerRadius="10" Padding="20">
     <StackPanel>
      <DockPanel><Button x:Name="Report" DockPanel.Dock="Right" Content="" Margin="10,0,0,0"/><TextBlock x:Name="Progress" Text="" FontWeight="SemiBold" VerticalAlignment="Center" TextWrapping="Wrap"/></DockPanel>
      <ProgressBar x:Name="Spinner" Height="3" Margin="0,12,0,0" Visibility="Collapsed" IsIndeterminate="True"/>
      <Expander x:Name="CheckDetails" Header="" Margin="0,10,0,0"><TextBox x:Name="Details" IsReadOnly="True" TextWrapping="Wrap" AcceptsReturn="True" VerticalScrollBarVisibility="Auto" Height="190" FontFamily="Consolas" FontSize="12" Margin="0,8,0,0"/></Expander>
     </StackPanel>
    </Border>
   </StackPanel>
  </ScrollViewer>
   </TabItem>
   <TabItem x:Name="ControlsTab" Header="">
    <ScrollViewer VerticalScrollBarVisibility="Auto">
     <StackPanel Margin="0,16,0,0">
      <Border Background="White" CornerRadius="10" Padding="20" Margin="0,0,0,14">
       <StackPanel>
        <TextBlock x:Name="ControlsHeading" FontSize="22" FontWeight="SemiBold" Margin="0,0,0,12"/>
        <WrapPanel><Button x:Name="ControlsToggle"/><Button x:Name="ControlsLaunch"/><Button x:Name="ReloadSettings"/><Button x:Name="ResetSettings"/></WrapPanel>
        <TextBlock x:Name="ControlsEffectState" FontWeight="SemiBold" Foreground="#1D4ED8" Margin="0,14,0,4"/>
        <TextBlock x:Name="ControlsRuntimeState" TextWrapping="Wrap" Foreground="#53647A"/>
        <TextBlock x:Name="FrameReadout" TextWrapping="Wrap" Foreground="#53647A" Margin="0,6,0,0"/>
        <TextBlock x:Name="ControlsHint" TextWrapping="Wrap" Foreground="#64748B" FontSize="12" Margin="0,10,0,0"/>
        <DockPanel Margin="0,12,0,0"><Button x:Name="ApplySettings" DockPanel.Dock="Right" Margin="10,0,0,0"/><TextBlock x:Name="SettingsState" FontWeight="SemiBold" VerticalAlignment="Center" TextWrapping="Wrap"/></DockPanel>
       </StackPanel>
      </Border>
      <StackPanel x:Name="KnobRows"/>
     </StackPanel>
    </ScrollViewer>
   </TabItem>
  </TabControl>
 </DockPanel>
</Window>
'@
$script:Window = [Windows.Markup.XamlReader]::Load([Xml.XmlNodeReader]::new($xaml))
$script:Ui = @{}
foreach($name in @('DllPath','GamePath','PythonPath','BrowseDll','BrowseGame','BrowsePython','FindPython','GetPython','Api','Mode','GameArgs','SteamId','Fossilize','Check','Dependencies','Install','Launch','Toggle','SteamSetup','SteamRestore','EffectState','RuntimeState','Report','Progress','Spinner','Details','CheckDetails','MainScroll','Subtitle','FilesHeading','DllLabel','GameLabel','PythonLabel','PythonHint','ConnectHeading','ApiLabel','ApiVulkan','ApiDxvk','ModeLabel','ModeDirect','ModeSteam','LaunchOptions','ArgsLabel','SteamIdLabel','FirstTestHint','CompareHeading','FpsHint','LanguageLabel','Language')) {
    $script:Ui[$name] = $script:Window.FindName($name)
    if($null -eq $script:Ui[$name]) { throw "Missing control $name" }
}
foreach($name in @('Pages','SetupTab','ControlsTab','ControlsHeading','ControlsToggle','ControlsLaunch','ReloadSettings','ResetSettings','ControlsEffectState','ControlsRuntimeState','FrameReadout','ControlsHint','ApplySettings','SettingsState','KnobRows')) {
    $script:Ui[$name]=$script:Window.FindName($name)
    if($null -eq $script:Ui[$name]) { throw "Missing control $name" }
}
$script:Window.MinHeight=540
$script:Window.Height=[Math]::Min(900,[Windows.SystemParameters]::WorkArea.Height-36)
$script:Window.Width=[Math]::Min(940,[Windows.SystemParameters]::WorkArea.Width-36)


# Windows UI culture selects the initial language. Switching never edits a profile,
# runtime setting or Steam option; only app-owned labels and messages change.
$script:Language=$(if([Globalization.CultureInfo]::CurrentUICulture.TwoLetterISOLanguageName -eq 'ru'){'ru'}else{'en'})
$script:ChangingLanguage=$false
$script:ProgressKey='ChooseFiles'
$script:RuntimeKey='RuntimeInitial'
$script:RuntimeValues=@()
$script:LastStatus=$null
$script:Strings=@{
 en=@{
  Title='DLSS-NR — setup and launch'; LanguageLabel='Language'; LanguageTip='Change the setup language.'
  Subtitle='Setup and test launch on Intel Xe2'; FilesHeading='1. Choose your files'
  DllLabel='Your NVIDIA DLL'; GameLabel='Game executable'; PythonLabel='64-bit Python'; Browse='Browse…'
  DllTip='Choose your own nvngx_dlssnr.dll. The file is not downloaded or included in the package.'
  GameTip='The exact 64-bit .exe for Vulkan or an existing DXVK configuration.'
  PythonTip='Choose native 64-bit Windows Python, not the MinGW/MSYS2 interpreter.'
  FindPython='Find Python'; GetPython='Python website'
  PythonHint='Python is usually detected automatically. If it is missing, install Windows x64 Python from python.org.'
  ConnectHeading='2. Connect NR to your game'; ApiLabel='Graphics API'; ApiVulkan='Vulkan'; ApiDxvk='DirectX 9–11 with DXVK'
  ModeLabel='Launch mode'; ModeDirect='Direct'; ModeSteam='Through Steam'; LaunchOptions='Launch options'
  ArgsLabel='Game arguments (optional)'; ArgsTip='Ordinary game arguments; shell commands are not executed here.'
  SteamIdLabel='Steam App ID'; SteamIdTip='Detected from the installed game; you can also enter it manually.'
  Fossilize='Steam shader-cache workaround for NR'
  FossilizeTip='Disables Fossilize only for the NR launch. Steam Overlay remains available.'
  FirstTestHint='For the first test, choose an 800×450 game window. The network starts at scale 0.4. DXVK must already be configured; setup does not install it.'
  Check='Check'; Dependencies='Prepare Python'; Install='Install NR'; CompareHeading='3. Launch and compare'
  Launch='Launch game'; Enable='Enable NR'; Disable='Disable NR'; SteamSetup='Configure Steam'; SteamRestore='Restore Steam'
  EffectOn='Effect enabled'; EffectOff='Effect disabled'
  RuntimeInitial='The game has not been tested yet. After enabling NR, wait for processed frames.'
  FpsHint='NR adds processing to every frame and can reduce FPS substantially. Compare the same scene with the effect enabled and disabled.'
  Report='Save report'; CheckDetails='Check details'; ChooseFiles='Choose your game and your own DLL.'
  PythonMissing='Python was not found. Install Windows x64 Python or choose python.exe.'
  ActionFailed='The action could not be completed.'; NoResult='The action finished without a result. Open the details.'
  ResultMissing='No result was produced.'; FixChecks='Please fix the reported items. Open the check details.'
  StatusUnavailable='Status is currently unavailable. The last known effect state has been kept.'
  Network='Network: {0}×{1}.'; Frames='Frames'; TailFrames='Frames in the latest log section'
  Processing='The game is being processed. {0}: {1}; rejected: {2}. {3}'
  Historical='The log contains {0} processed frames; rejected: {1}. Fresh frames are not currently confirmed.'
  WaitingFrames='No fresh processed frames yet. Enter a game scene and enable NR.'
  BusyClose='Please wait for the current action to finish.'
  PickDll='Your nvngx_dlssnr.dll'; PickGame='64-bit game executable'; PickPython='64-bit Windows Python'
  DllFilter='NVIDIA DLSS-NR DLL|nvngx_dlssnr.dll|DLL files|*.dll'; GameFilter='Game executable|*.exe'
  PythonFilter='Python interpreter|python.exe;python3.exe|Executable|*.exe'; ReportFilter='JSON report|*.json'
  SteamConfirm='Steam will exit normally and reopen. Close your Steam games first. Only the selected game''s launch options will change; use Restore Steam to return them.'
  SteamTitle='Configure NR launch'; ReportTitle='Save diagnostic report'
  'Loading.discover'='Looking for suitable Python…'; 'Loading.check'='Checking files and dependencies…'
  'Loading.dependencies'='Preparing local Python…'; 'Loading.install'='Extracting weights and installing NR…'
  'Loading.launch'='Launching the game…'; 'Loading.steam-setup'='Configuring Steam launch…'
  'Loading.steam-restore'='Restoring Steam launch options…'; 'Loading.on'='Enabling NR…'; 'Loading.off'='Disabling NR…'
  'Loading.report'='Saving report…'; 'Loading.save'='Saving settings…'
  'Done.discover'='Python found. Choose your game and DLL.'; 'Done.check'='Checks passed. You can install NR.'
  'Done.dependencies'='Python is ready. Now install NR.'; 'Done.install'='NR installed. Launch the game through setup.'
  'Done.launch'='Launch requested. Enter a game scene.'; 'Done.steam-setup'='Steam configured. Launch the game through setup.'
  'Done.steam-restore'='Original Steam launch options restored.'; 'Done.on'='NR enabled. Wait for the first processed frames.'
  'Done.off'='NR disabled.'; 'Done.report'='Report saved.'; 'Done.save'='Settings saved.'
  SetupTab='Setup'; ControlsTab='NR controls'; ControlsHeading='Live NR controls'
  ControlsHint='Changes save automatically after you finish adjusting. The daemon reads them between frames; no game restart is needed. Requested scale and the actual padded network size may differ.'
  ReloadSettings='Reload'; ResetSettings='Reset all'; ApplySettings='Apply now'; DefaultKnob='Default'
  SettingsLoading='Loading NR settings…'; SettingsReady='Current settings loaded.'; SettingsUnsaved='Changes waiting to be saved…'
  SettingsSaved='Saved. Used on the next processed frame.'; SettingsFailed='Settings could not be saved. See Setup → Check details.'
  UnsavedClose='Some NR changes were not saved. Close and discard those changes?'
  InvalidSetting='Enter a finite number within the supported range.'; FrameReadout='Last neural frame: output {0}×{1}; network {3}×{4}; processing {2} ms. This is not game FPS.'
  NetworkGroup='Network size'; LookGroup='Look'; StabilityGroup='Stability'
  'Loading.settings'='Loading NR settings…'; 'Loading.settings-save'='Saving NR controls…'; 'Loading.settings-reset'='Restoring NR defaults…'
  'Done.settings'='NR settings loaded.'; 'Done.settings-save'='NR settings saved.'; 'Done.settings-reset'='NR defaults restored.'
  'Knob.render_scale'='Render scale'; 'Knob.min_extent'='Minimum network side'; 'Knob.profile'='Profile'; 'Knob.intensity'='Intensity'
  'Knob.detail_strength'='Detail strength'; 'Knob.colour_strength'='Colour strength'; 'Knob.temporal'='Temporal history'
  'Knob.hold'='Hold unchanged pixels'; 'Knob.release'='Release moving pixels'; 'Knob.cut_limit'='Scene-cut threshold'
  'Hint.render_scale'='Fraction of each dimension processed by the network. Lower is cheaper, with coarser detail; minimum padding can limit the reduction.'
  'Hint.min_extent'='Minimum padded network side. 320 matches the vendor; 128 costs less for small frames and changes the surrounding mirrored context.'
  'Hint.profile'='Standard adds texture; natural and cinematic preserve more highlights; neutral greatly reduces the effect.'
  'Hint.intensity'='Blend strength: 0 keeps the game image, 1 uses the model result, above 1 exaggerates the change.'
  'Hint.detail_strength'='Strength of fine detail. Zero keeps only broad tone changes; above 1 adds more sharpening.'
  'Hint.colour_strength'='Strength of tone and colour changes. Higher values can reduce saturation; zero preserves the game tone.'
  'Hint.temporal'='How much previous output is fed back. Zero forgets history and draws frames independently.'
  'Hint.hold'='Keep the previous result where the game pixels did not change, reducing flicker in still areas.'
  'Hint.release'='Pixel change in levels of 255 that releases history. 16–24 is a useful starting range; zero disables this protection.'
  'Hint.cut_limit'='Average frame change that discards history for a scene cut. Lower reacts sooner; 1 never cuts.'
  'Profile.standard'='Standard'; 'Profile.natural'='Natural'; 'Profile.cinematic'='Cinematic'; 'Profile.neutral'='Neutral'
 }
 ru=@{
  Title='DLSS-NR — установка и запуск'; LanguageLabel='Язык'; LanguageTip='Выберите язык мастера.'
  Subtitle='Установка и пробный запуск на Intel Xe2'; FilesHeading='1. Выберите файлы'
  DllLabel='Ваша NVIDIA DLL'; GameLabel='Программа игры'; PythonLabel='64-битный Python'; Browse='Обзор…'
  DllTip='Выберите собственную nvngx_dlssnr.dll. Файл не скачивается и не входит в пакет.'
  GameTip='Точный 64-битный .exe для Vulkan или уже настроенного DXVK.'
  PythonTip='Выберите обычный 64-битный Windows Python, а не интерпретатор MinGW/MSYS2.'
  FindPython='Найти Python'; GetPython='Сайт Python'
  PythonHint='Python обычно определяется автоматически. Если его нет, установите Windows x64 Python с python.org.'
  ConnectHeading='2. Подключите NR к игре'; ApiLabel='Графический API'; ApiVulkan='Vulkan'; ApiDxvk='DirectX 9–11 с DXVK'
  ModeLabel='Запуск'; ModeDirect='Напрямую'; ModeSteam='Через Steam'; LaunchOptions='Параметры запуска'
  ArgsLabel='Аргументы игры (необязательно)'; ArgsTip='Обычные аргументы игры; команды оболочки здесь не выполняются.'
  SteamIdLabel='Steam App ID'; SteamIdTip='Определяется по установленной игре; можно указать вручную.'
  Fossilize='Обход Steam shader-cache для NR'
  FossilizeTip='Отключает Fossilize только для запуска с NR. Steam Overlay остаётся доступен.'
  FirstTestHint='Для первого теста выберите в игре окно 800×450. Сеть начнёт с масштаба 0.4. DXVK должен быть уже настроен; мастер его не устанавливает.'
  Check='Проверить'; Dependencies='Подготовить Python'; Install='Установить NR'; CompareHeading='3. Запустите и сравните'
  Launch='Запустить игру'; Enable='Включить NR'; Disable='Выключить NR'; SteamSetup='Настроить Steam'; SteamRestore='Вернуть Steam'
  EffectOn='Эффект включён'; EffectOff='Эффект выключен'
  RuntimeInitial='Игра ещё не проверена. После включения NR дождитесь обработанных кадров.'
  FpsHint='NR добавляет обработку каждого кадра и может заметно снизить FPS. Сравните одну сцену с эффектом и без него.'
  Report='Сохранить отчёт'; CheckDetails='Подробности проверки'; ChooseFiles='Выберите игру и свою DLL.'
  PythonMissing='Python не найден. Установите Windows x64 Python или выберите python.exe.'
  ActionFailed='Не удалось выполнить действие.'; NoResult='Действие завершилось без результата. Откройте подробности.'
  ResultMissing='Результат отсутствует.'; FixChecks='Нужно исправить отмеченные пункты. Откройте подробности проверки.'
  StatusUnavailable='Статус сейчас недоступен. Последнее известное состояние эффекта сохранено.'
  Network='Сеть: {0}×{1}.'; Frames='Кадров'; TailFrames='Кадров в последнем участке лога'
  Processing='Игра обрабатывается. {0}: {1}; отклонено: {2}. {3}'
  Historical='В логе {0} обработанных кадров; отклонено: {1}. Свежие кадры сейчас не подтверждены.'
  WaitingFrames='Свежие обработанные кадры ещё не появились. Войдите в игровую сцену и включите NR.'
  BusyClose='Дождитесь завершения текущего действия.'
  PickDll='Ваша nvngx_dlssnr.dll'; PickGame='64-битная программа игры'; PickPython='64-битный Windows Python'
  DllFilter='NVIDIA DLSS-NR DLL|nvngx_dlssnr.dll|Файлы DLL|*.dll'; GameFilter='Программа игры|*.exe'
  PythonFilter='Интерпретатор Python|python.exe;python3.exe|Программа|*.exe'; ReportFilter='Отчёт JSON|*.json'
  SteamConfirm='Steam будет закрыт обычным способом и снова открыт. Перед этим завершите игры Steam. Изменятся только параметры запуска выбранной игры; их можно вернуть кнопкой «Вернуть Steam».'
  SteamTitle='Настройка запуска NR'; ReportTitle='Сохранить диагностический отчёт'
  'Loading.discover'='Ищем подходящий Python…'; 'Loading.check'='Проверяем файлы и зависимости…'
  'Loading.dependencies'='Готовим локальный Python…'; 'Loading.install'='Извлекаем веса и устанавливаем NR…'
  'Loading.launch'='Запускаем игру…'; 'Loading.steam-setup'='Настраиваем запуск в Steam…'
  'Loading.steam-restore'='Возвращаем параметры Steam…'; 'Loading.on'='Включаем NR…'; 'Loading.off'='Выключаем NR…'
  'Loading.report'='Сохраняем отчёт…'; 'Loading.save'='Сохраняем параметры…'
  'Done.discover'='Python найден. Выберите игру и DLL.'; 'Done.check'='Проверка пройдена. Можно установить NR.'
  'Done.dependencies'='Python подготовлен. Теперь установите NR.'; 'Done.install'='NR установлен. Запустите игру через мастер.'
  'Done.launch'='Запуск запрошен. Войдите в игровую сцену.'; 'Done.steam-setup'='Steam настроен. Запустите игру через мастер.'
  'Done.steam-restore'='Обычные параметры Steam восстановлены.'; 'Done.on'='NR включён. Дождитесь первых обработанных кадров.'
  'Done.off'='NR выключен.'; 'Done.report'='Отчёт сохранён.'; 'Done.save'='Параметры сохранены.'
  SetupTab='Установка'; ControlsTab='Настройки NR'; ControlsHeading='Настройки NR во время игры'
  ControlsHint='Изменения сохраняются автоматически после регулировки. Демон читает их между кадрами; перезапуск игры не нужен. Выбранный масштаб и фактический размер сети с дополнением могут различаться.'
  ReloadSettings='Перечитать'; ResetSettings='Сбросить всё'; ApplySettings='Применить'; DefaultKnob='Сброс'
  SettingsLoading='Загружаем настройки NR…'; SettingsReady='Текущие настройки загружены.'; SettingsUnsaved='Изменения ожидают сохранения…'
  SettingsSaved='Сохранено. Используется со следующего обработанного кадра.'; SettingsFailed='Не удалось сохранить. См. «Установка» → «Подробности проверки».'
  UnsavedClose='Некоторые изменения NR не сохранены. Закрыть окно и отбросить эти изменения?'
  InvalidSetting='Введите конечное число в допустимом диапазоне.'; FrameReadout='Последний нейронный кадр: выход {0}×{1}; сеть {3}×{4}; обработка {2} мс. Это не FPS игры.'
  NetworkGroup='Размер сети'; LookGroup='Изображение'; StabilityGroup='Стабилизация'
  'Loading.settings'='Загружаем настройки NR…'; 'Loading.settings-save'='Сохраняем настройки NR…'; 'Loading.settings-reset'='Возвращаем настройки NR…'
  'Done.settings'='Настройки NR загружены.'; 'Done.settings-save'='Настройки NR сохранены.'; 'Done.settings-reset'='Настройки NR сброшены.'
  'Knob.render_scale'='Масштаб сети'; 'Knob.min_extent'='Минимальная сторона сети'; 'Knob.profile'='Профиль'; 'Knob.intensity'='Интенсивность'
  'Knob.detail_strength'='Сила деталей'; 'Knob.colour_strength'='Сила цвета'; 'Knob.temporal'='История кадров'
  'Knob.hold'='Удержание неподвижных пикселей'; 'Knob.release'='Освобождение при движении'; 'Knob.cut_limit'='Порог смены сцены'
  'Hint.render_scale'='Доля каждой стороны, которую обрабатывает сеть. Меньше — дешевле и грубее детали; минимальное дополнение может ограничить уменьшение.'
  'Hint.min_extent'='Минимальная сторона сети с дополнением. 320 соответствует NVIDIA; 128 дешевле на малых кадрах и меняет зеркальное окружение сцены.'
  'Hint.profile'='Стандартный добавляет текстуру; естественный и кинематографический лучше сохраняют светлые участки; нейтральный сильно уменьшает эффект.'
  'Hint.intensity'='Смешивание: 0 оставляет картинку игры, 1 использует результат модели, выше 1 преувеличивает изменения.'
  'Hint.detail_strength'='Сила мелких деталей. Ноль оставляет только тональные изменения; выше 1 усиливает резкость.'
  'Hint.colour_strength'='Сила тональных и цветовых изменений. Большие значения могут снижать насыщенность; ноль сохраняет тон игры.'
  'Hint.temporal'='Доля предыдущего результата в новом кадре. Ноль забывает историю и обрабатывает кадры независимо.'
  'Hint.hold'='Сохраняет предыдущий результат там, где пиксели игры не менялись, уменьшая мерцание неподвижных участков.'
  'Hint.release'='Изменение пикселя в уровнях из 255, которое освобождает историю. Начните с 16–24; ноль выключает эту защиту.'
  'Hint.cut_limit'='Среднее изменение кадра для сброса истории при смене сцены. Меньше — быстрее реагирует; 1 никогда не сбрасывает.'
  'Profile.standard'='Стандартный'; 'Profile.natural'='Естественный'; 'Profile.cinematic'='Кинематографический'; 'Profile.neutral'='Нейтральный'
 }
}
$script:TextBindings=@{
 Subtitle='Subtitle'; FilesHeading='FilesHeading'; DllLabel='DllLabel'; GameLabel='GameLabel'; PythonLabel='PythonLabel'
 PythonHint='PythonHint'; ConnectHeading='ConnectHeading'; ApiLabel='ApiLabel'; ModeLabel='ModeLabel'
 ArgsLabel='ArgsLabel'; SteamIdLabel='SteamIdLabel'; FirstTestHint='FirstTestHint'; CompareHeading='CompareHeading'
 FpsHint='FpsHint'; LanguageLabel='LanguageLabel'; ControlsHeading='ControlsHeading'; ControlsHint='ControlsHint'
}
$script:ContentBindings=@{
 BrowseDll='Browse'; BrowseGame='Browse'; BrowsePython='Browse'; FindPython='FindPython'; GetPython='GetPython'
 ApiVulkan='ApiVulkan'; ApiDxvk='ApiDxvk'; ModeDirect='ModeDirect'; ModeSteam='ModeSteam'; Fossilize='Fossilize'
 Check='Check'; Dependencies='Dependencies'; Install='Install'; Launch='Launch'; SteamSetup='SteamSetup'
 SteamRestore='SteamRestore'; Report='Report'; ControlsLaunch='Launch'; ReloadSettings='ReloadSettings'; ResetSettings='ResetSettings'; ApplySettings='ApplySettings'
}
$script:HeaderBindings=@{LaunchOptions='LaunchOptions'; CheckDetails='CheckDetails';SetupTab='SetupTab';ControlsTab='ControlsTab'}
$script:TipBindings=@{DllPath='DllTip'; GamePath='GameTip'; PythonPath='PythonTip'; GameArgs='ArgsTip'; SteamId='SteamIdTip'; Fossilize='FossilizeTip'; Language='LanguageTip'}
function T([string]$Key) {
    if(-not $script:Strings[$script:Language].ContainsKey($Key)) { throw "Missing translation: $Key" }
    return [string]$script:Strings[$script:Language][$Key]
}
function Set-Progress([string]$Key) {
    $script:ProgressKey=$Key
    $script:Ui.Progress.Text=T $Key
}
function Set-RuntimeText([string]$Key,[object[]]$Values=@()) {
    $script:RuntimeKey=$Key
    $script:RuntimeValues=$Values
    $script:Ui.RuntimeState.Text=[string]::Format((T $Key),$Values)
    $script:Ui.ControlsRuntimeState.Text=$script:Ui.RuntimeState.Text
}
function Set-SettingsState([string]$Key) {
    $script:SettingsStateKey=$Key
    $script:Ui.SettingsState.Text=T $Key
}
function Format-KnobValue($Value) {
    if($Value -is [string]) { return $Value }
    return ([double]$Value).ToString('0.####',[Globalization.CultureInfo]::InvariantCulture)
}
function Set-KnobDisplay([string]$Name,$Value) {
    $row=$script:KnobUi[$Name]
    $wasLoading=$script:LoadingSettings; $script:LoadingSettings=$true
    try {
        $row.Value=$Value
        if($row.Choice) {
            foreach($item in $row.Choice.Items) { if([string]$item.Tag -eq [string]$Value) { $row.Choice.SelectedItem=$item; break } }
        } else {
            $row.Input.Text=Format-KnobValue $Value
            $row.Input.BorderBrush=[Windows.Media.Brushes]::LightGray
            $row.Slider.Value=[Math]::Min([Math]::Max([double]$Value,[double]$row.Model.low),[double]$row.Model.high)
        }
    } finally { $script:LoadingSettings=$wasLoading }
}
function Change-Knob([string]$Name,$Value) {
    if($script:LoadingSettings) { return }
    $row=$script:KnobUi[$Name]
    if($row.Model.kind -eq 'number') { $Value=[Math]::Round([double]$Value,4) }
    if($null -ne $row.Value -and (Format-KnobValue $row.Value) -eq (Format-KnobValue $Value)) { Set-KnobDisplay $Name $Value; return }
    Set-KnobDisplay $Name $Value
    $script:PendingSettings[$Name]=$Value
    $script:SettingsDue=[DateTime]::UtcNow.AddMilliseconds(450)
    Set-SettingsState 'SettingsUnsaved'
}
function Commit-KnobInput($InputControl) {
    if($script:LoadingSettings) { return }
    $name=[string]$InputControl.Tag; $row=$script:KnobUi[$name]; $number=0.0
    $valid=[double]::TryParse($InputControl.Text.Trim().Replace(',','.'),[Globalization.NumberStyles]::Float,[Globalization.CultureInfo]::InvariantCulture,[ref]$number)
    if(-not $valid -or [double]::IsNaN($number) -or [double]::IsInfinity($number) -or $number -lt $row.Model.runtime_low -or $number -gt $row.Model.runtime_high) {
        $InputControl.BorderBrush=[Windows.Media.Brushes]::IndianRed
        Set-SettingsState 'InvalidSetting'
        return
    }
    Change-Knob $name $number
}
function Commit-FocusedKnob {
    $focused=[Windows.Input.Keyboard]::FocusedElement
    if($focused -is [Windows.Controls.TextBox] -and $script:KnobUi.ContainsKey([string]$focused.Tag)) { Commit-KnobInput $focused }
}
function Update-KnobLanguage {
    foreach($name in $script:KnobUi.Keys) {
        $row=$script:KnobUi[$name]
        $row.Label.Text=(T ('Knob.'+$name))+' ('+$name+')'
        $row.Hint.Text=T ('Hint.'+$name)
        $defaultLabel=$(if($row.Model.kind -eq 'choice'){T ('Profile.'+[string]$row.Model.default)}else{Format-KnobValue $row.Model.default})
        $row.Default.Content=(T 'DefaultKnob')+' '+$defaultLabel
        $row.Default.ToolTip=T ('Hint.'+$name)
        if($row.Choice) {
            [Windows.Automation.AutomationProperties]::SetName($row.Choice,(T ('Knob.'+$name)))
            foreach($item in $row.Choice.Items) { $item.Content=T ('Profile.'+[string]$item.Tag) }
        } else {
            $range=''+$row.Model.runtime_low+' – '+$row.Model.runtime_high
            $row.Input.ToolTip=(T ('Knob.'+$name))+' ['+$range+']'
            $row.Slider.ToolTip=T ('Hint.'+$name)
            [Windows.Automation.AutomationProperties]::SetName($row.Input,(T ('Knob.'+$name)))
            [Windows.Automation.AutomationProperties]::SetName($row.Slider,(T ('Knob.'+$name)))
        }
    }
    foreach($group in @($script:Ui.KnobRows.Children)) { $group.Child.Children[0].Text=T ([string]$group.Tag) }
}
function Create-KnobControls($Schema) {
    $script:KnobUi=@{}; $script:Ui.KnobRows.Children.Clear()
    $script:LoadingSettings=$true
    try {
        $currentGroup=''; $groupPanel=$null
        foreach($model in $Schema.knobs) {
            $name=[string]$model.name
            $group=$(if($name -in @('render_scale','min_extent')){'NetworkGroup'}elseif($name -in @('profile','intensity','detail_strength','colour_strength')){'LookGroup'}else{'StabilityGroup'})
            if($group -ne $currentGroup) {
                $card=[Windows.Controls.Border]::new(); $card.Background=[Windows.Media.Brushes]::White; $card.CornerRadius=[Windows.CornerRadius]::new(10); $card.Padding=[Windows.Thickness]::new(20); $card.Margin=[Windows.Thickness]::new(0,0,0,14); $card.Tag=$group
                $groupPanel=[Windows.Controls.StackPanel]::new(); $card.Child=$groupPanel
                $heading=[Windows.Controls.TextBlock]::new(); $heading.FontSize=18; $heading.FontWeight=[Windows.FontWeights]::SemiBold; $heading.Margin=[Windows.Thickness]::new(0,0,0,12)
                $null=$groupPanel.Children.Add($heading); $null=$script:Ui.KnobRows.Children.Add($card); $currentGroup=$group
            }
            $container=[Windows.Controls.StackPanel]::new(); $container.Margin=[Windows.Thickness]::new(0,0,0,15)
            $grid=[Windows.Controls.Grid]::new()
            foreach($width in @('245','*','76','155')) { $column=[Windows.Controls.ColumnDefinition]::new(); $column.Width=[Windows.GridLengthConverter]::new().ConvertFromString($width); $null=$grid.ColumnDefinitions.Add($column) }
            $label=[Windows.Controls.TextBlock]::new(); $label.TextWrapping='Wrap'; $label.VerticalAlignment='Center'; $label.FontSize=13; $null=$grid.Children.Add($label)
            $defaultButton=[Windows.Controls.Button]::new(); $defaultButton.Tag=$name; $defaultButton.FontSize=12; $defaultButton.Padding=[Windows.Thickness]::new(6,7,6,7); $defaultButton.Margin=[Windows.Thickness]::new(10,0,0,0); [Windows.Controls.Grid]::SetColumn($defaultButton,3); $null=$grid.Children.Add($defaultButton)
            $defaultButton.Add_Click({param($sender,$eventArgs); Reset-NrSettings @([string]$sender.Tag)})
            $row=@{Model=$model;Label=$label;Default=$defaultButton;Input=$null;Slider=$null;Choice=$null}
            if($model.kind -eq 'choice') {
                $choice=[Windows.Controls.ComboBox]::new(); $choice.Tag=$name
                foreach($value in $model.choices) { $item=[Windows.Controls.ComboBoxItem]::new(); $item.Tag=[string]$value; $null=$choice.Items.Add($item) }
                [Windows.Controls.Grid]::SetColumn($choice,1); [Windows.Controls.Grid]::SetColumnSpan($choice,2); $null=$grid.Children.Add($choice); $row.Choice=$choice
                [Windows.Automation.AutomationProperties]::SetAutomationId($choice,'NrChoice_'+$name)
                $choice.Add_SelectionChanged({param($sender,$eventArgs); if($sender.SelectedItem) { Change-Knob ([string]$sender.Tag) ([string]$sender.SelectedItem.Tag) }})
            } else {
                $slider=[Windows.Controls.Slider]::new(); $slider.Tag=$name; $slider.Minimum=[double]$model.low; $slider.Maximum=[double]$model.high; $slider.TickFrequency=[double]$model.step; $slider.SmallChange=[double]$model.step; $slider.LargeChange=[double]$model.step*5; $slider.IsSnapToTickEnabled=$true; $slider.VerticalAlignment='Center'; $slider.Margin=[Windows.Thickness]::new(8,0,14,0)
                [Windows.Controls.Grid]::SetColumn($slider,1); $null=$grid.Children.Add($slider); $row.Slider=$slider
                [Windows.Automation.AutomationProperties]::SetAutomationId($slider,'NrSlider_'+$name)
                $inputBox=[Windows.Controls.TextBox]::new(); $inputBox.Tag=$name; $inputBox.Padding=[Windows.Thickness]::new(6); $inputBox.HorizontalContentAlignment='Right'; [Windows.Controls.Grid]::SetColumn($inputBox,2); $null=$grid.Children.Add($inputBox); $row.Input=$inputBox
                [Windows.Automation.AutomationProperties]::SetAutomationId($inputBox,'NrInput_'+$name)
                $slider.Add_ValueChanged({param($sender,$eventArgs); Change-Knob ([string]$sender.Tag) $sender.Value})
                $inputBox.Add_LostKeyboardFocus({param($sender,$eventArgs); Commit-KnobInput $sender})
                $inputBox.Add_KeyDown({param($sender,$eventArgs); if($eventArgs.Key -eq [Windows.Input.Key]::Enter) { Commit-KnobInput $sender; $eventArgs.Handled=$true }})
            }
            $hint=[Windows.Controls.TextBlock]::new(); $hint.TextWrapping='Wrap'; $hint.FontSize=12; $hint.Foreground=[Windows.Media.Brushes]::SlateGray; $hint.Margin=[Windows.Thickness]::new(0,5,0,0); $row.Hint=$hint
            $null=$container.Children.Add($grid); $null=$container.Children.Add($hint); $null=$groupPanel.Children.Add($container); $script:KnobUi[$name]=$row
        }
        Update-KnobLanguage
    } finally { $script:LoadingSettings=$false }
}
function Apply-NrSettings($Result) {
    if(-not $script:KnobUi.Count) { Create-KnobControls $Result }
    foreach($model in $Result.knobs) {
        $name=[string]$model.name
        if(-not $script:PendingSettings.ContainsKey($name)) { Set-KnobDisplay $name $Result.settings.$name }
    }
    Set-SettingsState $(if($script:PendingSettings.Count){'SettingsUnsaved'}else{'SettingsReady'})
}
function Save-NrSettings {
    if($script:Busy -or -not $script:PendingSettings.Count) { return }
    $patch=@{}; foreach($key in $script:PendingSettings.Keys) { $patch[$key]=$script:PendingSettings[$key] }
    $script:PendingSettings=@{}; $script:SettingsDue=[DateTime]::MaxValue
    Start-Bridge 'settings-save' '' $false @{settings=$patch}
    if($script:Busy) { $script:Busy | Add-Member -NotePropertyName SettingsPatch -NotePropertyValue $patch }
    else { foreach($key in $patch.Keys) { $script:PendingSettings[$key]=$patch[$key] }; Set-SettingsState 'SettingsFailed' }
}
function Reset-NrSettings([string[]]$Names=@()) {
    if($script:Busy) { return }
    if($Names.Count) { foreach($name in $Names) { $script:PendingSettings.Remove($name) } } else { $script:PendingSettings=@{} }
    $resetPayload=$(if($Names.Count){@{reset=@($Names)}}else{$null})
    Start-Bridge 'settings-reset' '' $false $resetPayload
}
function Restore-SettingsFailure($Job) {
    if($Job -and $Job.Action -in @('settings','settings-save','settings-reset')) { Set-SettingsState 'SettingsFailed' }
    if($Job -and $Job.SettingsPatch) {
        foreach($key in $Job.SettingsPatch.Keys) { if(-not $script:PendingSettings.ContainsKey($key)) { $script:PendingSettings[$key]=$Job.SettingsPatch[$key] } }
        $script:SettingsDue=[DateTime]::MaxValue
    }
}
function Set-Language([string]$Language) {
    if($Language -notin @('en','ru')) { throw 'Unsupported setup language' }
    $script:ChangingLanguage=$true
    try {
        $script:Language=$Language
        $script:Ui.Language.SelectedIndex=$(if($Language -eq 'ru'){1}else{0})
        $script:Window.Title=T 'Title'
        foreach($name in $script:TextBindings.Keys) { $script:Ui[$name].Text=T $script:TextBindings[$name] }
        foreach($name in $script:ContentBindings.Keys) { $script:Ui[$name].Content=T $script:ContentBindings[$name] }
        foreach($name in $script:HeaderBindings.Keys) { $script:Ui[$name].Header=T $script:HeaderBindings[$name] }
        foreach($name in $script:TipBindings.Keys) { $script:Ui[$name].ToolTip=T $script:TipBindings[$name] }
        Update-KnobLanguage
        Set-SettingsState $script:SettingsStateKey
        Set-Progress $script:ProgressKey
        $runtimeKey=$script:RuntimeKey
        if($script:LastStatus) {
            Show-Status $script:LastStatus
            if($runtimeKey -eq 'StatusUnavailable') { Set-RuntimeText 'StatusUnavailable' }
        } else {
            $script:Ui.EffectState.Text=T 'EffectOff'
            $script:Ui.Toggle.Content=T 'Enable'
            $script:Ui.ControlsToggle.Content=T 'Enable'
            $script:Ui.ControlsEffectState.Text=T 'EffectOff'
            Set-RuntimeText $runtimeKey $script:RuntimeValues
        }
    } finally { $script:ChangingLanguage=$false }
}

function Quote-Argument([string]$Value) {
    # Windows CreateProcess quoting, not CMD/PowerShell source interpolation.
    return '"' + [regex]::Replace([regex]::Replace($Value, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'
}
function Read-Json([string]$Path) {
    if(Test-Path -LiteralPath $Path) { return ([IO.File]::ReadAllText($Path,[Text.Encoding]::UTF8) | ConvertFrom-Json) }
    return $null
}
function Write-Json([string]$Path, $Value) {
    [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 10), [Text.UTF8Encoding]::new($false))
}
function Profile-FromWindow {
    $dll = $script:Ui.DllPath.Text.Trim()
    return [ordered]@{root=$script:Root; python=$script:Ui.PythonPath.Text.Trim(); game_exe=$script:Ui.GamePath.Text.Trim(); dll=$(if($dll){$dll}else{$null}); api=$(if($script:Ui.Api.SelectedIndex -eq 1){'dxvk'}else{'vulkan'}); game_args_raw=$script:Ui.GameArgs.Text; disable_fossilize=[bool]$script:Ui.Fossilize.IsChecked; launch_mode=$(if($script:Ui.Mode.SelectedIndex -eq 1){'steam'}else{'direct'}); steam_app_id=$script:Ui.SteamId.Text.Trim()}
}
function Apply-Profile($Profile) {
    if($null -eq $Profile) { return }
    $script:LastProfile=$Profile
    $script:Ui.PythonPath.Text=[string]$Profile.python
    $script:Ui.GamePath.Text=[string]$Profile.game_exe
    $script:Ui.DllPath.Text=[string]$Profile.dll
    $script:Ui.Api.SelectedIndex=$(if($Profile.api -eq 'dxvk'){1}else{0})
    $script:Ui.Mode.SelectedIndex=$(if($Profile.launch_mode -eq 'steam'){1}else{0})
    $script:Ui.SteamId.Text=[string]$Profile.steam_app_id
    $script:Ui.Fossilize.IsChecked=[bool]$Profile.disable_fossilize
    if($Profile.game_args) { $script:Ui.GameArgs.Text=(@($Profile.game_args | ForEach-Object { Quote-Argument ([string]$_) }) -join ' ') }
}
function Set-Busy([bool]$Busy) {
    foreach($name in @('DllPath','GamePath','PythonPath','Api','Mode','GameArgs','SteamId','Fossilize','BrowseDll','BrowseGame','BrowsePython','FindPython','Check','Dependencies','Install','Launch','Toggle','SteamSetup','SteamRestore','Report','ControlsToggle','ControlsLaunch','ReloadSettings','ResetSettings','ApplySettings')) { $script:Ui[$name].IsEnabled=-not $Busy }
    $script:Ui.KnobRows.IsEnabled=(-not $Busy -or ($script:Busy -and $script:Busy.Action -eq 'settings-save'))
    $script:Ui.Spinner.Visibility=$(if($Busy){'Visible'}else{'Collapsed'})
}
function Start-Bridge([string]$Action,[string]$Destination='', [bool]$Quiet=$false, $Extra=$null) {
    $python=$script:Ui.PythonPath.Text.Trim()
    if($Action -eq 'discover') { $python=$script:BootPython }
    if(-not $python -or -not (Test-Path -LiteralPath $python -PathType Leaf)) {
        if(-not $Quiet) { Set-Progress 'PythonMissing' }
        return
    }
    try {
        [IO.Directory]::CreateDirectory($script:Work) | Out-Null
        $id=[Guid]::NewGuid().ToString('N')
        $output=Join-Path $script:Work ('result-'+$id+'.json')
        $request=Join-Path $script:Work ('request-'+$id+'.json')
        $stdout=Join-Path $script:Work ('action-'+$id+'.stdout.log')
        $stderr=Join-Path $script:Work ('action-'+$id+'.stderr.log')
        $args=@($script:Bridge,$Action,'--root',$script:Root,'--output',$output)
        if($Action -ne 'discover') {
            $requestValues=Profile-FromWindow
            if($Extra) { foreach($key in $Extra.Keys) { $requestValues[$key]=$Extra[$key] } }
            Write-Json $request $requestValues; $args+=@('--input',$request)
        }
        if($Destination) { $args+=@('--destination',$Destination) }
        $proc=Start-Process -FilePath $python -ArgumentList (($args | ForEach-Object { Quote-Argument $_ }) -join ' ') -WorkingDirectory $script:Root -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
        # Windows PS 5 needs the native handle retained before the first HasExited poll.
        $null=$proc.Handle
        $job=[pscustomobject]@{Process=$proc;Action=$Action;Output=$output;Request=$request;Stdout=$stdout;Stderr=$stderr;Started=[DateTime]::UtcNow}
        if($Quiet) { $script:StatusJob=$job } else {
            $script:Busy=$job; Set-Busy $true
            Set-Progress ('Loading.'+$Action)
        }
    } catch { if(-not $Quiet) { Set-Progress 'ActionFailed'; $script:Ui.Details.Text=$_.Exception.Message; Set-Busy $false } }
}
function Show-Status($Value) {
    $enabled=[bool]$Value.trigger_exists
    if($null -ne $Value.effect_on) { $enabled=[bool]$Value.effect_on }
    $script:LastStatus=$Value
    $script:Ui.EffectState.Text=$(if($enabled){T 'EffectOn'}else{T 'EffectOff'})
    $script:Ui.Toggle.Content=$(if($enabled){T 'Disable'}else{T 'Enable'})
    $script:Ui.ControlsToggle.Content=$script:Ui.Toggle.Content
    $script:Ui.ControlsEffectState.Text=$script:Ui.EffectState.Text
    $count=$Value.processed
    if($null -eq $count -and $Value.counts) { $count=$Value.counts.processed }
    $refused=$Value.rejected
    if($null -eq $refused -and $Value.counts) { $refused=$Value.counts.rejected }
    if($null -eq $count) { $count=0 }
    if($null -eq $refused) { $refused=0 }
    $shape=''
    if($Value.network_shape -and $Value.network_shape.Count -eq 2) { $shape=[string]::Format((T 'Network'),$Value.network_shape[0],$Value.network_shape[1]) }
    $kind=$(if($Value.counts_complete -eq $false){T 'TailFrames'}else{T 'Frames'})
    if($Value.fresh_frames -or $Value.game_connected) {
        Set-RuntimeText 'Processing' @($kind,$count,$refused,$shape)
    } elseif([int]$count -gt 0) {
        Set-RuntimeText 'Historical' @($count,$refused)
    } else { Set-RuntimeText 'WaitingFrames' }
    $script:Ui.FrameReadout.Text=''
    if($Value.last_frame -and $Value.network_shape -and $Value.network_shape.Count -eq 2) {
        $frame=$Value.last_frame
        $script:Ui.FrameReadout.Text=[string]::Format((T 'FrameReadout'),$frame.width,$frame.height,[Math]::Round([double]$frame.seconds*1000),$Value.network_shape[0],$Value.network_shape[1])
    }
}
function Finish-Bridge($Job,[bool]$Quiet) {
    $Job.Process.WaitForExit()
    if(-not $Quiet) {
        $exitCode=$Job.Process.ExitCode
        $measuredCode=$(if($null -eq $exitCode){'unavailable'}else{[string]$exitCode})
        [IO.File]::WriteAllText(($Job.Output+'.exit.txt'),$measuredCode,[Text.UTF8Encoding]::new($false))
    }
    $value=Read-Json $Job.Output
    if($Quiet) {
        if($value -and $value.ok) { Show-Status $value }
        elseif($value) { Set-RuntimeText 'StatusUnavailable' }
        foreach($path in @($Job.Output,$Job.Request,$Job.Stdout,$Job.Stderr)) {
            if($path -and (Test-Path -LiteralPath $path)) { Remove-Item -LiteralPath $path -ErrorAction SilentlyContinue }
        }
        $script:StatusJob=$null; return
    }
    if(-not $value) {
        Restore-SettingsFailure $Job
        Set-Progress 'NoResult'
        $script:Ui.Details.Text=$(if(Test-Path -LiteralPath $Job.Stderr){[IO.File]::ReadAllText($Job.Stderr)}else{T 'ResultMissing'})
    } else {
        $script:Ui.Details.Text=($value | ConvertTo-Json -Depth 10)
        if($value.ok) {
            Set-Progress ('Done.'+$Job.Action)
            if($value.profile -and $Job.Action -in @('discover','dependencies','install','steam-setup')) { Apply-Profile $value.profile }
            if($Job.Action -eq 'discover' -and -not $value.profile -and $value.candidates.Count) { $script:Ui.PythonPath.Text=[string]$value.candidates[0] }
            if($Job.Action -eq 'install') { $script:Installed=$true }
            if($Job.Action -in @('discover','install')) {
                $script:SettingsReload=$true
                if($Job.Action -eq 'install' -or $value.profile) { $script:Ui.Pages.SelectedItem=$script:Ui.ControlsTab }
            }
            if($Job.Action -in @('settings','settings-save','settings-reset')) {
                Apply-NrSettings $value
                if($Job.Action -ne 'settings' -and -not $script:PendingSettings.Count) { Set-SettingsState 'SettingsSaved' }
            }
            if($Job.Action -in @('on','off','status')) { Show-Status $value }
        } else {
            Set-Progress 'FixChecks'
            $script:Ui.CheckDetails.IsExpanded=$true
            Restore-SettingsFailure $Job
        }
    }
    $script:Busy=$null; Set-Busy $false
    if($Job.Action -eq 'discover') { $script:Ui.MainScroll.ScrollToTop() }
}
function Pick-File([string]$Title,[string]$Filter,$Control) {
    $dialog=[Microsoft.Win32.OpenFileDialog]::new(); $dialog.Title=$Title; $dialog.Filter=$Filter
    if($dialog.ShowDialog($script:Window)) { $Control.Text=$dialog.FileName }
}
function Find-BootPython {
    $candidates=[Collections.Generic.List[string]]::new()
    if($env:NR_PYTHON) { $candidates.Add($env:NR_PYTHON) }
    foreach($registryPath in @('HKCU:\Software\Python\PythonCore\*\InstallPath','HKLM:\Software\Python\PythonCore\*\InstallPath')) {
        foreach($key in @(Get-Item -Path $registryPath -ErrorAction SilentlyContinue)) {
            $path=$key.GetValue('ExecutablePath'); if(-not $path) { $path=Join-Path $key.GetValue('') 'python.exe' }; if($path) { $candidates.Add($path) }
        }
    }
    foreach($name in @('python','python3')) { $cmd=Get-Command $name -CommandType Application -ErrorAction SilentlyContinue; if($cmd -and $cmd.Source -notlike '*\WindowsApps\*') { $candidates.Add($cmd.Source) } }
    $uvRoot=Join-Path $env:APPDATA 'uv\python'
    if(Test-Path -LiteralPath $uvRoot) { foreach($item in @(Get-ChildItem -LiteralPath $uvRoot -Filter 'cpython-*-windows-x86_64-none' -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending)) { $candidates.Add((Join-Path $item.FullName 'python.exe')) } }
    $saved=Read-Json (Join-Path $script:Root 'work\windows-profile.json')
    if($saved -and $saved.python -and (Test-Path -LiteralPath $saved.python -PathType Leaf)) { Apply-Profile $saved; return [string]$saved.python }
    foreach($candidate in $candidates) { if(Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate } }
    return ''
}


Set-Language $script:Language
if($SelfTest) {
    if($ControlSchema) {
        $schema=Read-Json $ControlSchema
        Apply-NrSettings $schema
        if($script:KnobUi.Count -ne 10) { throw 'Expected all ten Linux panel controls' }
        foreach($model in $schema.knobs) {
            $row=$script:KnobUi[[string]$model.name]
            if($model.kind -eq 'number' -and ($row.Slider.Minimum -ne $model.low -or $row.Slider.Maximum -ne $model.high -or $row.Slider.TickFrequency -ne $model.step)) { throw "Wrong slider range: $($model.name)" }
        }
    }
    foreach($key in $script:Strings.en.Keys) {
        if(-not $script:Strings.ru.ContainsKey($key) -or -not $script:Strings.en[$key] -or -not $script:Strings.ru[$key]) { throw "Incomplete translation: $key" }
        if($script:Strings.en[$key] -match '[А-Яа-яЁё]') { throw "Russian text in English translation: $key" }
    }
    if($script:Strings.en.Count -ne $script:Strings.ru.Count) { throw 'Translation key counts differ' }
    $autoLanguage=$script:Language
    foreach($language in @('en','ru')) {
        Set-Language $language
        if($script:KnobUi.Count) {
            foreach($name in $script:KnobUi.Keys) { if($script:KnobUi[$name].Label.Text -notlike ((T ('Knob.'+$name))+'*')) { throw "Untranslated control $name" } }
            if($script:PendingSettings.Count) { throw 'Loading or changing language changed settings' }
        }
        foreach($binding in @($script:TextBindings,$script:ContentBindings,$script:HeaderBindings,$script:TipBindings)) {
            foreach($name in $binding.Keys) { if(-not (T $binding[$name])) { throw "Missing UI translation for $name" } }
        }
        Show-Status ([pscustomobject]@{trigger_exists=$true;processed=12;rejected=0;network_shape=@(320,320);fresh_frames=$true;counts_complete=$true})
        if($script:Ui.Toggle.Content -ne (T 'Disable') -or $script:Ui.RuntimeState.Text -notmatch '320×320') { throw 'Localized enabled status failed' }
        Set-Language $(if($language -eq 'en'){'ru'}else{'en'})
        if($script:Ui.Toggle.Content -ne (T 'Disable')) { throw 'Language switch changed the displayed effect state' }
        Show-Status ([pscustomobject]@{trigger_exists=$false;processed=12;rejected=1;fresh_frames=$false})
        if($script:Ui.Toggle.Content -ne (T 'Enable')) { throw 'Localized disabled status failed' }
        Set-Progress 'Loading.install'
        Set-RuntimeText 'StatusUnavailable'
        Set-Language $language
        if($script:Ui.Progress.Text -ne (T 'Loading.install') -or $script:Ui.RuntimeState.Text -ne (T 'StatusUnavailable')) { throw 'Dynamic messages did not switch languages' }
    }
    Write-Output ('WPF layout loaded; '+$script:Ui.Count+' named controls; '+$script:KnobUi.Count+' live NR controls. EN/RU coverage: '+$script:Strings.en.Count+' keys; automatic language: '+$autoLanguage+'.')
    exit 0
}
$script:Ui.Language.Add_SelectionChanged({
    if(-not $script:ChangingLanguage -and $script:Ui.Language.SelectedItem) { Set-Language ([string]$script:Ui.Language.SelectedItem.Tag) }
})

$script:Ui.BrowseDll.Add_Click({ Pick-File (T 'PickDll') (T 'DllFilter') $script:Ui.DllPath })
$script:Ui.BrowseGame.Add_Click({ Pick-File (T 'PickGame') (T 'GameFilter') $script:Ui.GamePath })
$script:Ui.BrowsePython.Add_Click({ Pick-File (T 'PickPython') (T 'PythonFilter') $script:Ui.PythonPath })
$script:Ui.GetPython.Add_Click({ Start-Process 'https://www.python.org/downloads/windows/' })
$script:Ui.FindPython.Add_Click({ $script:BootPython=Find-BootPython; if($script:BootPython) { Start-Bridge 'discover' } else { Set-Progress 'PythonMissing' } })
$script:Ui.Check.Add_Click({ Start-Bridge 'check' })
$script:Ui.Dependencies.Add_Click({ Start-Bridge 'dependencies' })
$script:Ui.Install.Add_Click({ Start-Bridge 'install' })
$script:Ui.Launch.Add_Click({ Start-Bridge 'launch' })
$script:Ui.Toggle.Add_Click({ $action=$(if($script:Ui.Toggle.Content -eq (T 'Disable')){'off'}else{'on'}); Start-Bridge $action })
$script:Ui.ControlsToggle.Add_Click({ $action=$(if($script:Ui.ControlsToggle.Content -eq (T 'Disable')){'off'}else{'on'}); Start-Bridge $action })
$script:Ui.ControlsLaunch.Add_Click({ Start-Bridge 'launch' })
$script:Ui.ReloadSettings.Add_Click({ $script:PendingSettings=@{}; $script:SettingsDue=[DateTime]::MaxValue; Start-Bridge 'settings' })
$script:Ui.ResetSettings.Add_Click({ Reset-NrSettings })
$script:Ui.ApplySettings.Add_Click({ Commit-FocusedKnob; Save-NrSettings })
$script:Ui.SteamSetup.Add_Click({
    if([Windows.MessageBox]::Show($script:Window,(T 'SteamConfirm'),(T 'SteamTitle'),'OKCancel','Information') -eq 'OK') { Start-Bridge 'steam-setup' }
})
$script:Ui.SteamRestore.Add_Click({ Start-Bridge 'steam-restore' })
$script:Ui.Report.Add_Click({
    $dialog=[Microsoft.Win32.SaveFileDialog]::new(); $dialog.Title=T 'ReportTitle'; $dialog.Filter=T 'ReportFilter'; $dialog.FileName='DLSS-NR-report.json'
    if($dialog.ShowDialog($script:Window)) { Start-Bridge 'report' $dialog.FileName }
})
$script:Timer=[Windows.Threading.DispatcherTimer]::new(); $script:Timer.Interval=[TimeSpan]::FromMilliseconds(500)
$script:Timer.Add_Tick({
    try {
        if($script:Busy -and $script:Busy.Process.HasExited) { Finish-Bridge $script:Busy $false }
        if($script:StatusJob -and $script:StatusJob.Process.HasExited) { Finish-Bridge $script:StatusJob $true }
        if(-not $script:Busy -and $script:SettingsReload) { $script:SettingsReload=$false; Start-Bridge 'settings' }
        if(-not $script:Busy -and $script:PendingSettings.Count -and [DateTime]::UtcNow -ge $script:SettingsDue) {
            $dragging=$false
            foreach($row in $script:KnobUi.Values) { if($row.Slider -and $row.Slider.IsMouseCaptureWithin) { $dragging=$true; break } }
            if(-not $dragging) { Save-NrSettings }
        }
        $script:TickCount++
        if($script:TickCount % 10 -eq 0 -and -not $script:Busy -and -not $script:StatusJob -and $script:Ui.GamePath.Text -and $script:Ui.PythonPath.Text) { Start-Bridge 'status' '' $true }
    } catch { $script:Ui.Details.Text=$_.Exception.Message; Restore-SettingsFailure $script:Busy; $script:Busy=$null; $script:StatusJob=$null; Set-Busy $false; Set-Progress 'ActionFailed' }
})
$script:Window.Add_Closing({
    param($sender,$eventArgs)
    Commit-FocusedKnob
    if($script:Busy) { $eventArgs.Cancel=$true; Set-Progress 'BusyClose' }
    elseif($script:PendingSettings.Count -and $script:SettingsStateKey -ne 'SettingsFailed') { $eventArgs.Cancel=$true; Save-NrSettings; Set-Progress 'BusyClose' }
    elseif($script:PendingSettings.Count) { $eventArgs.Cancel=([Windows.MessageBox]::Show($script:Window,(T 'UnsavedClose'),(T 'Title'),'YesNo','Warning') -ne 'Yes') }
})
$script:Window.Add_Closed({ $script:Timer.Stop() })
$script:Window.Add_ContentRendered({
    if(-not $script:Ready) {
        $script:Ready=$true
        $script:BootPython=Find-BootPython
        if($script:BootPython) { if(-not $script:Ui.PythonPath.Text) { $script:Ui.PythonPath.Text=$script:BootPython }; Start-Bridge 'discover' }
        else { Set-Progress 'PythonMissing' }
    }
})
$script:Timer.Start()
$null=$script:Window.ShowDialog()
