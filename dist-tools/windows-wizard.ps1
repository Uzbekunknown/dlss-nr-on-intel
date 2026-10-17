param([string]$Root = '', [switch]$SelfTest, [string]$ControlSchema = '')
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName PresentationFramework, PresentationCore, WindowsBase, System.Windows.Forms
# Drawn by the CPU: the window is text and buttons, and on an integrated GPU the game and the
# network share the device with it. Hardware rendering held 30 MB of GPU memory and 45 MB of RAM.
[Windows.Media.RenderOptions]::ProcessRenderMode = [Windows.Interop.RenderMode]::SoftwareOnly
if (-not $Root) { $Root = [IO.Path]::GetDirectoryName($PSCommandPath) }
$script:Root = [IO.Path]::GetFullPath($Root)
$script:Bridge = Join-Path $script:Root 'scripts\windows_wizard.py'
$script:Work = Join-Path $script:Root 'work\windows-wizard'
$script:Busy = $null
$script:StatusJob = $null
$script:TickCount = 0
$script:StatusDue = $false
$script:Ready = $false
$script:Installed = $false
$script:LastProfile = $null
$script:BootPython = ''
$script:KnobUi = @{}
$script:PendingSettings = @{}
$script:LoadingSettings = $false
$script:SettingsReload = $false
$script:SpeedTestDue = $false
$script:PacingDue = $false
$script:ApplyingProfile = $false
$script:SettingsDue = [DateTime]::MaxValue
$script:SettingsStateKey = 'SettingsLoading'

[xml]$xaml = @'
<Window xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
 Title="DLSS-NR" Width="780" Height="700" MinWidth="640" MinHeight="480" WindowStartupLocation="CenterScreen"
 FontFamily="Segoe UI" FontSize="13" Background="{DynamicResource Bg}" Foreground="{DynamicResource Text}">
 <Window.Resources>
  <!-- The theme's colours. Set-Theme replaces these brushes; everything below refers to them. -->
  <SolidColorBrush x:Key="Bg" Color="#F3F5F9"/>
  <SolidColorBrush x:Key="Card" Color="#FFFFFF"/>
  <SolidColorBrush x:Key="CardBorder" Color="#E3E8EF"/>
  <SolidColorBrush x:Key="Text" Color="#1A2433"/>
  <SolidColorBrush x:Key="Muted" Color="#5B6B80"/>
  <SolidColorBrush x:Key="Accent" Color="#2563EB"/>
  <SolidColorBrush x:Key="AccentText" Color="#FFFFFF"/>
  <SolidColorBrush x:Key="Field" Color="#FFFFFF"/>
  <SolidColorBrush x:Key="FieldBorder" Color="#CBD5E1"/>
  <SolidColorBrush x:Key="Hover" Color="#EEF2F7"/>
  <SolidColorBrush x:Key="Selected" Color="#E0E9FB"/>
  <SolidColorBrush x:Key="Track" Color="#D5DCE6"/>
  <SolidColorBrush x:Key="Good" Color="#15803D"/>
  <SolidColorBrush x:Key="Bad" Color="#DC2626"/>
  <Style x:Key="CardBox" TargetType="Border">
   <Setter Property="Background" Value="{DynamicResource Card}"/><Setter Property="BorderBrush" Value="{DynamicResource CardBorder}"/>
   <Setter Property="BorderThickness" Value="1"/><Setter Property="CornerRadius" Value="8"/><Setter Property="Padding" Value="14,12"/><Setter Property="Margin" Value="0,0,0,10"/>
  </Style>
  <Style x:Key="Heading" TargetType="TextBlock"><Setter Property="FontSize" Value="15"/><Setter Property="FontWeight" Value="SemiBold"/><Setter Property="Margin" Value="0,0,0,8"/></Style>
  <Style x:Key="Note" TargetType="TextBlock"><Setter Property="Foreground" Value="{DynamicResource Muted}"/><Setter Property="FontSize" Value="12"/><Setter Property="TextWrapping" Value="Wrap"/></Style>
  <Style x:Key="FieldLabel" TargetType="TextBlock"><Setter Property="VerticalAlignment" Value="Center"/><Setter Property="Margin" Value="0,0,8,6"/><Setter Property="TextTrimming" Value="CharacterEllipsis"/></Style>
  <Style TargetType="Button">
   <Setter Property="Foreground" Value="{DynamicResource Text}"/><Setter Property="Background" Value="{DynamicResource Field}"/><Setter Property="BorderBrush" Value="{DynamicResource FieldBorder}"/>
   <Setter Property="Padding" Value="12,5"/><Setter Property="Margin" Value="0,0,6,6"/><Setter Property="Cursor" Value="Hand"/><Setter Property="FocusVisualStyle" Value="{x:Null}"/>
   <Setter Property="Template"><Setter.Value><ControlTemplate TargetType="Button">
    <Border x:Name="Bd" Background="{TemplateBinding Background}" BorderBrush="{TemplateBinding BorderBrush}" BorderThickness="1" CornerRadius="6" Padding="{TemplateBinding Padding}">
     <ContentPresenter HorizontalAlignment="Center" VerticalAlignment="Center"/></Border>
    <ControlTemplate.Triggers>
     <Trigger Property="IsMouseOver" Value="True"><Setter TargetName="Bd" Property="Background" Value="{DynamicResource Hover}"/></Trigger>
     <Trigger Property="IsKeyboardFocused" Value="True"><Setter TargetName="Bd" Property="BorderBrush" Value="{DynamicResource Accent}"/></Trigger>
     <Trigger Property="IsEnabled" Value="False"><Setter Property="Opacity" Value="0.45"/></Trigger>
    </ControlTemplate.Triggers>
   </ControlTemplate></Setter.Value></Setter>
  </Style>
  <Style x:Key="Primary" TargetType="Button" BasedOn="{StaticResource {x:Type Button}}">
   <Setter Property="Foreground" Value="{DynamicResource AccentText}"/><Setter Property="Background" Value="{DynamicResource Accent}"/><Setter Property="BorderBrush" Value="{DynamicResource Accent}"/>
   <Setter Property="Template"><Setter.Value><ControlTemplate TargetType="Button">
    <Border x:Name="Bd" Background="{TemplateBinding Background}" BorderBrush="{TemplateBinding BorderBrush}" BorderThickness="1" CornerRadius="6" Padding="{TemplateBinding Padding}">
     <ContentPresenter HorizontalAlignment="Center" VerticalAlignment="Center"/></Border>
    <ControlTemplate.Triggers>
     <Trigger Property="IsMouseOver" Value="True"><Setter TargetName="Bd" Property="Opacity" Value="0.88"/></Trigger>
     <Trigger Property="IsKeyboardFocused" Value="True"><Setter TargetName="Bd" Property="BorderBrush" Value="{DynamicResource Text}"/></Trigger>
     <Trigger Property="IsEnabled" Value="False"><Setter Property="Opacity" Value="0.45"/></Trigger>
    </ControlTemplate.Triggers>
   </ControlTemplate></Setter.Value></Setter>
  </Style>
  <Style x:Key="Icon" TargetType="Button">
   <Setter Property="FontFamily" Value="Segoe Fluent Icons, Segoe MDL2 Assets"/><Setter Property="FontSize" Value="13"/>
   <Setter Property="Foreground" Value="{DynamicResource Muted}"/><Setter Property="Width" Value="28"/><Setter Property="Height" Value="28"/>
   <Setter Property="Cursor" Value="Hand"/><Setter Property="FocusVisualStyle" Value="{x:Null}"/>
   <Setter Property="Template"><Setter.Value><ControlTemplate TargetType="Button">
    <Border x:Name="Bd" Background="Transparent" BorderBrush="Transparent" BorderThickness="1" CornerRadius="6"><ContentPresenter HorizontalAlignment="Center" VerticalAlignment="Center"/></Border>
    <ControlTemplate.Triggers>
     <Trigger Property="IsMouseOver" Value="True"><Setter TargetName="Bd" Property="Background" Value="{DynamicResource Hover}"/><Setter Property="Foreground" Value="{DynamicResource Text}"/></Trigger>
     <Trigger Property="IsKeyboardFocused" Value="True"><Setter TargetName="Bd" Property="BorderBrush" Value="{DynamicResource Accent}"/></Trigger>
     <Trigger Property="IsEnabled" Value="False"><Setter Property="Opacity" Value="0.4"/></Trigger>
    </ControlTemplate.Triggers>
   </ControlTemplate></Setter.Value></Setter>
  </Style>
  <Style x:Key="Link" TargetType="Button">
   <Setter Property="Foreground" Value="{DynamicResource Accent}"/><Setter Property="Cursor" Value="Hand"/><Setter Property="FocusVisualStyle" Value="{x:Null}"/>
   <Setter Property="Template"><Setter.Value><ControlTemplate TargetType="Button">
    <Border x:Name="Bd" Background="Transparent" Padding="4,2" CornerRadius="4" BorderThickness="1" BorderBrush="Transparent"><ContentPresenter VerticalAlignment="Center"/></Border>
    <ControlTemplate.Triggers>
     <Trigger Property="IsMouseOver" Value="True"><Setter TargetName="Bd" Property="Background" Value="{DynamicResource Hover}"/></Trigger>
     <Trigger Property="IsKeyboardFocused" Value="True"><Setter TargetName="Bd" Property="BorderBrush" Value="{DynamicResource Accent}"/></Trigger>
     <Trigger Property="IsEnabled" Value="False"><Setter Property="Opacity" Value="0.45"/></Trigger>
    </ControlTemplate.Triggers>
   </ControlTemplate></Setter.Value></Setter>
  </Style>
  <Style TargetType="TextBox">
   <Setter Property="Foreground" Value="{DynamicResource Text}"/><Setter Property="Background" Value="{DynamicResource Field}"/><Setter Property="BorderBrush" Value="{DynamicResource FieldBorder}"/>
   <Setter Property="CaretBrush" Value="{DynamicResource Text}"/><Setter Property="SelectionBrush" Value="{DynamicResource Accent}"/>
   <Setter Property="Padding" Value="6,4"/><Setter Property="VerticalContentAlignment" Value="Center"/>
   <Setter Property="Template"><Setter.Value><ControlTemplate TargetType="TextBox">
    <Border x:Name="Bd" Background="{TemplateBinding Background}" BorderBrush="{TemplateBinding BorderBrush}" BorderThickness="1" CornerRadius="6">
     <ScrollViewer x:Name="PART_ContentHost" Margin="{TemplateBinding Padding}" VerticalAlignment="{TemplateBinding VerticalContentAlignment}"/></Border>
    <ControlTemplate.Triggers>
     <Trigger Property="IsKeyboardFocused" Value="True"><Setter TargetName="Bd" Property="BorderBrush" Value="{DynamicResource Accent}"/></Trigger>
     <Trigger Property="IsEnabled" Value="False"><Setter Property="Opacity" Value="0.5"/></Trigger>
    </ControlTemplate.Triggers>
   </ControlTemplate></Setter.Value></Setter>
  </Style>
  <Style TargetType="ComboBox">
   <Setter Property="Foreground" Value="{DynamicResource Text}"/><Setter Property="FocusVisualStyle" Value="{x:Null}"/><Setter Property="MinHeight" Value="28"/>
   <Setter Property="Template"><Setter.Value><ControlTemplate TargetType="ComboBox">
    <Grid>
     <ToggleButton Focusable="False" ClickMode="Press" IsChecked="{Binding IsDropDownOpen, Mode=TwoWay, RelativeSource={RelativeSource TemplatedParent}}">
      <ToggleButton.Template><ControlTemplate TargetType="ToggleButton">
       <Border x:Name="Bd" Background="{DynamicResource Field}" BorderBrush="{DynamicResource FieldBorder}" BorderThickness="1" CornerRadius="6">
        <Path HorizontalAlignment="Right" VerticalAlignment="Center" Margin="0,0,10,0" Data="M0,0 L4,4 L8,0" Stroke="{DynamicResource Muted}" StrokeThickness="1.5"/></Border>
       <ControlTemplate.Triggers><Trigger Property="IsMouseOver" Value="True"><Setter TargetName="Bd" Property="Background" Value="{DynamicResource Hover}"/></Trigger></ControlTemplate.Triggers>
      </ControlTemplate></ToggleButton.Template>
     </ToggleButton>
     <Border x:Name="FocusRing" BorderBrush="{DynamicResource Accent}" BorderThickness="1" CornerRadius="6" Visibility="Collapsed" IsHitTestVisible="False"/>
     <ContentPresenter Margin="9,0,26,0" VerticalAlignment="Center" IsHitTestVisible="False" Content="{TemplateBinding SelectionBoxItem}" ContentTemplate="{TemplateBinding SelectionBoxItemTemplate}"/>
     <Popup x:Name="PART_Popup" IsOpen="{TemplateBinding IsDropDownOpen}" Placement="Bottom" AllowsTransparency="True" Focusable="False" PopupAnimation="None">
      <Border Background="{DynamicResource Card}" BorderBrush="{DynamicResource FieldBorder}" BorderThickness="1" CornerRadius="6" Margin="0,2,0,0" Padding="2" MinWidth="{Binding ActualWidth, RelativeSource={RelativeSource TemplatedParent}}">
       <ScrollViewer MaxHeight="260" HorizontalScrollBarVisibility="Disabled" VerticalScrollBarVisibility="Auto"><StackPanel IsItemsHost="True" KeyboardNavigation.DirectionalNavigation="Contained"/></ScrollViewer>
      </Border>
     </Popup>
    </Grid>
    <ControlTemplate.Triggers>
     <Trigger Property="IsKeyboardFocusWithin" Value="True"><Setter TargetName="FocusRing" Property="Visibility" Value="Visible"/></Trigger>
     <Trigger Property="IsEnabled" Value="False"><Setter Property="Opacity" Value="0.5"/></Trigger>
    </ControlTemplate.Triggers>
   </ControlTemplate></Setter.Value></Setter>
  </Style>
  <Style TargetType="ComboBoxItem">
   <Setter Property="Foreground" Value="{DynamicResource Text}"/><Setter Property="FocusVisualStyle" Value="{x:Null}"/>
   <Setter Property="Template"><Setter.Value><ControlTemplate TargetType="ComboBoxItem">
    <Border x:Name="Bd" Background="Transparent" Padding="8,5" CornerRadius="4"><ContentPresenter/></Border>
    <ControlTemplate.Triggers>
     <Trigger Property="IsSelected" Value="True"><Setter TargetName="Bd" Property="Background" Value="{DynamicResource Selected}"/></Trigger>
     <Trigger Property="IsHighlighted" Value="True"><Setter TargetName="Bd" Property="Background" Value="{DynamicResource Hover}"/></Trigger>
    </ControlTemplate.Triggers>
   </ControlTemplate></Setter.Value></Setter>
  </Style>
  <Style TargetType="CheckBox">
   <Setter Property="Foreground" Value="{DynamicResource Text}"/><Setter Property="FocusVisualStyle" Value="{x:Null}"/><Setter Property="Cursor" Value="Hand"/>
   <Setter Property="Template"><Setter.Value><ControlTemplate TargetType="CheckBox">
    <StackPanel Orientation="Horizontal" Background="Transparent">
     <Border x:Name="Box" Width="16" Height="16" CornerRadius="4" BorderThickness="1" BorderBrush="{DynamicResource FieldBorder}" Background="{DynamicResource Field}" VerticalAlignment="Center">
      <Path x:Name="Mark" Data="M3,7.5 L6,10.5 L11.5,4" Stroke="{DynamicResource AccentText}" StrokeThickness="2" Visibility="Collapsed"/></Border>
     <ContentPresenter Margin="8,0,0,0" VerticalAlignment="Center"/>
    </StackPanel>
    <ControlTemplate.Triggers>
     <Trigger Property="IsChecked" Value="True"><Setter TargetName="Box" Property="Background" Value="{DynamicResource Accent}"/><Setter TargetName="Box" Property="BorderBrush" Value="{DynamicResource Accent}"/><Setter TargetName="Mark" Property="Visibility" Value="Visible"/></Trigger>
     <Trigger Property="IsKeyboardFocused" Value="True"><Setter TargetName="Box" Property="BorderBrush" Value="{DynamicResource Accent}"/></Trigger>
    </ControlTemplate.Triggers>
   </ControlTemplate></Setter.Value></Setter>
  </Style>
  <Style x:Key="TrackFill" TargetType="RepeatButton">
   <Setter Property="Focusable" Value="False"/><Setter Property="IsTabStop" Value="False"/>
   <Setter Property="Template"><Setter.Value><ControlTemplate TargetType="RepeatButton"><Border Background="Transparent"><Border Height="4" CornerRadius="2" Background="{DynamicResource Accent}"/></Border></ControlTemplate></Setter.Value></Setter>
  </Style>
  <Style x:Key="TrackRest" TargetType="RepeatButton">
   <Setter Property="Focusable" Value="False"/><Setter Property="IsTabStop" Value="False"/>
   <Setter Property="Template"><Setter.Value><ControlTemplate TargetType="RepeatButton"><Border Background="Transparent"><Border Height="4" CornerRadius="2" Background="{DynamicResource Track}"/></Border></ControlTemplate></Setter.Value></Setter>
  </Style>
  <!-- A slider takes the keyboard when clicked: the arrows move it a step, Page Up/Down five. -->
  <Style TargetType="Slider">
   <Setter Property="Focusable" Value="True"/><Setter Property="IsMoveToPointEnabled" Value="True"/><Setter Property="FocusVisualStyle" Value="{x:Null}"/><Setter Property="Cursor" Value="Hand"/>
   <Setter Property="Template"><Setter.Value><ControlTemplate TargetType="Slider">
    <Grid Height="24" Background="Transparent"><Track x:Name="PART_Track">
     <Track.DecreaseRepeatButton><RepeatButton Style="{StaticResource TrackFill}" Command="{x:Static Slider.DecreaseLarge}"/></Track.DecreaseRepeatButton>
     <Track.IncreaseRepeatButton><RepeatButton Style="{StaticResource TrackRest}" Command="{x:Static Slider.IncreaseLarge}"/></Track.IncreaseRepeatButton>
     <Track.Thumb><Thumb Focusable="False"><Thumb.Template><ControlTemplate TargetType="Thumb">
      <Grid Width="20" Height="20" Background="Transparent">
       <Ellipse x:Name="Ring" Fill="{DynamicResource Accent}" Opacity="0"/>
       <Ellipse Width="14" Height="14" Fill="{DynamicResource Card}" Stroke="{DynamicResource Accent}" StrokeThickness="2"/>
      </Grid>
      <ControlTemplate.Triggers>
       <Trigger Property="IsMouseOver" Value="True"><Setter TargetName="Ring" Property="Opacity" Value="0.2"/></Trigger>
       <DataTrigger Binding="{Binding IsKeyboardFocused, RelativeSource={RelativeSource AncestorType=Slider}}" Value="True"><Setter TargetName="Ring" Property="Opacity" Value="0.35"/></DataTrigger>
      </ControlTemplate.Triggers>
     </ControlTemplate></Thumb.Template></Thumb></Track.Thumb>
    </Track></Grid>
    <ControlTemplate.Triggers><Trigger Property="IsEnabled" Value="False"><Setter Property="Opacity" Value="0.45"/></Trigger></ControlTemplate.Triggers>
   </ControlTemplate></Setter.Value></Setter>
  </Style>
  <Style TargetType="TabControl">
   <Setter Property="Template"><Setter.Value><ControlTemplate TargetType="TabControl">
    <DockPanel><TabPanel DockPanel.Dock="Top" IsItemsHost="True" Margin="0,0,0,10"/><ContentPresenter x:Name="PART_SelectedContentHost" ContentSource="SelectedContent"/></DockPanel>
   </ControlTemplate></Setter.Value></Setter>
  </Style>
  <Style TargetType="TabItem">
   <Setter Property="Foreground" Value="{DynamicResource Muted}"/><Setter Property="FocusVisualStyle" Value="{x:Null}"/><Setter Property="Cursor" Value="Hand"/>
   <Setter Property="Template"><Setter.Value><ControlTemplate TargetType="TabItem">
    <Border x:Name="Bd" Padding="12,5" Margin="0,0,4,0" CornerRadius="6" Background="Transparent" BorderThickness="1" BorderBrush="Transparent"><ContentPresenter ContentSource="Header"/></Border>
    <ControlTemplate.Triggers>
     <Trigger Property="IsMouseOver" Value="True"><Setter TargetName="Bd" Property="Background" Value="{DynamicResource Hover}"/></Trigger>
     <Trigger Property="IsSelected" Value="True"><Setter TargetName="Bd" Property="Background" Value="{DynamicResource Card}"/><Setter TargetName="Bd" Property="BorderBrush" Value="{DynamicResource CardBorder}"/><Setter Property="Foreground" Value="{DynamicResource Text}"/></Trigger>
     <Trigger Property="IsKeyboardFocused" Value="True"><Setter TargetName="Bd" Property="BorderBrush" Value="{DynamicResource Accent}"/></Trigger>
    </ControlTemplate.Triggers>
   </ControlTemplate></Setter.Value></Setter>
  </Style>
  <Style TargetType="Expander">
   <Setter Property="Foreground" Value="{DynamicResource Text}"/>
   <Setter Property="Template"><Setter.Value><ControlTemplate TargetType="Expander">
    <DockPanel>
     <ToggleButton DockPanel.Dock="Top" Cursor="Hand" FocusVisualStyle="{x:Null}" Foreground="{TemplateBinding Foreground}" Content="{TemplateBinding Header}" IsChecked="{Binding IsExpanded, Mode=TwoWay, RelativeSource={RelativeSource TemplatedParent}}">
      <ToggleButton.Template><ControlTemplate TargetType="ToggleButton">
       <Border x:Name="Bd" Background="Transparent" Padding="2,3" CornerRadius="4" BorderThickness="1" BorderBrush="Transparent">
        <StackPanel Orientation="Horizontal">
         <Path x:Name="Arrow" Width="8" Height="8" Margin="2,0,8,0" VerticalAlignment="Center" Data="M2,0 L6,4 L2,8" Stroke="{DynamicResource Muted}" StrokeThickness="1.5"/>
         <ContentPresenter VerticalAlignment="Center"/>
        </StackPanel>
       </Border>
       <ControlTemplate.Triggers>
        <Trigger Property="IsChecked" Value="True"><Setter TargetName="Arrow" Property="Data" Value="M0,2 L4,6 L8,2"/></Trigger>
        <Trigger Property="IsMouseOver" Value="True"><Setter TargetName="Bd" Property="Background" Value="{DynamicResource Hover}"/></Trigger>
        <Trigger Property="IsKeyboardFocused" Value="True"><Setter TargetName="Bd" Property="BorderBrush" Value="{DynamicResource Accent}"/></Trigger>
       </ControlTemplate.Triggers>
      </ControlTemplate></ToggleButton.Template>
     </ToggleButton>
     <ContentPresenter x:Name="Body" Visibility="Collapsed" Margin="0,6,0,0"/>
    </DockPanel>
    <ControlTemplate.Triggers><Trigger Property="IsExpanded" Value="True"><Setter TargetName="Body" Property="Visibility" Value="Visible"/></Trigger></ControlTemplate.Triggers>
   </ControlTemplate></Setter.Value></Setter>
  </Style>
  <Style TargetType="ScrollBar">
   <Setter Property="Width" Value="10"/><Setter Property="MinWidth" Value="10"/>
   <Setter Property="Template"><Setter.Value><ControlTemplate TargetType="ScrollBar">
    <Track x:Name="PART_Track" Orientation="{TemplateBinding Orientation}" IsDirectionReversed="True">
     <Track.Thumb><Thumb><Thumb.Template><ControlTemplate TargetType="Thumb"><Border Background="{DynamicResource Track}" CornerRadius="4" Margin="2"/></ControlTemplate></Thumb.Template></Thumb></Track.Thumb>
    </Track>
   </ControlTemplate></Setter.Value></Setter>
  </Style>
  <Style TargetType="ToolTip">
   <Setter Property="Background" Value="{DynamicResource Card}"/><Setter Property="Foreground" Value="{DynamicResource Text}"/><Setter Property="BorderBrush" Value="{DynamicResource FieldBorder}"/><Setter Property="Padding" Value="8,6"/>
   <Setter Property="ContentTemplate"><Setter.Value><DataTemplate><TextBlock Text="{Binding}" TextWrapping="Wrap" MaxWidth="380"/></DataTemplate></Setter.Value></Setter>
  </Style>
 </Window.Resources>
 <DockPanel Margin="16,12,16,10">
  <DockPanel DockPanel.Dock="Top" Margin="0,0,0,10">
   <StackPanel DockPanel.Dock="Right" Orientation="Horizontal" VerticalAlignment="Center">
    <ComboBox x:Name="Language" Width="110" ToolTip="">
     <ComboBoxItem Content="English" Tag="en"/>
     <ComboBoxItem Content="中文" Tag="zh"/>
     <ComboBoxItem Content="Русский" Tag="ru"/>
     <ComboBoxItem Content="Español" Tag="es"/>
    </ComboBox>
    <Button x:Name="Theme" Style="{StaticResource Icon}" Margin="6,0,0,0" ToolTip=""/>
   </StackPanel>
   <StackPanel Orientation="Horizontal" VerticalAlignment="Center">
    <TextBlock Text="DLSS-NR" FontSize="20" FontWeight="SemiBold" VerticalAlignment="Center"/>
    <TextBlock x:Name="Subtitle" Text="" Foreground="{DynamicResource Muted}" Margin="10,4,0,0" VerticalAlignment="Center"/>
   </StackPanel>
  </DockPanel>
  <StackPanel DockPanel.Dock="Bottom" Margin="0,6,0,0">
   <ProgressBar x:Name="Spinner" Height="2" Visibility="Collapsed" IsIndeterminate="False" Foreground="{DynamicResource Accent}" Background="{DynamicResource Track}" BorderThickness="0"/>
   <TextBlock x:Name="Progress" Text="" Foreground="{DynamicResource Muted}" TextWrapping="Wrap" Margin="2,5,0,0"/>
  </StackPanel>
  <TabControl x:Name="Pages">
   <TabItem x:Name="SetupTab" Header="">
    <ScrollViewer x:Name="MainScroll" VerticalScrollBarVisibility="Auto" HorizontalScrollBarVisibility="Disabled">
     <StackPanel>
      <Border Style="{StaticResource CardBox}">
       <StackPanel>
        <TextBlock x:Name="FilesHeading" Style="{StaticResource Heading}"/>
        <Grid><Grid.ColumnDefinitions><ColumnDefinition Width="140"/><ColumnDefinition Width="*"/><ColumnDefinition Width="Auto"/></Grid.ColumnDefinitions>
         <Grid.RowDefinitions><RowDefinition Height="Auto"/><RowDefinition Height="Auto"/><RowDefinition Height="Auto"/></Grid.RowDefinitions>
         <TextBlock x:Name="DllLabel" Style="{StaticResource FieldLabel}"/><TextBox x:Name="DllPath" Grid.Column="1" Margin="0,0,6,6" ToolTip=""/><Button x:Name="BrowseDll" Grid.Column="2" Margin="0,0,0,6"/>
         <TextBlock x:Name="GameLabel" Grid.Row="1" Style="{StaticResource FieldLabel}"/><TextBox x:Name="GamePath" Grid.Row="1" Grid.Column="1" Margin="0,0,6,6" ToolTip=""/><Button x:Name="BrowseGame" Grid.Row="1" Grid.Column="2" Margin="0,0,0,6"/>
         <TextBlock x:Name="PythonLabel" Grid.Row="2" Style="{StaticResource FieldLabel}"/><TextBox x:Name="PythonPath" Grid.Row="2" Grid.Column="1" Margin="0,0,6,6"/><Button x:Name="BrowsePython" Grid.Row="2" Grid.Column="2" Margin="0,0,0,6"/>
        </Grid>
        <WrapPanel Margin="140,0,0,0"><Button x:Name="FindPython" Margin="0,0,6,0"/><Button x:Name="GetPython" Margin="0"/></WrapPanel>
       </StackPanel>
      </Border>
      <Border Style="{StaticResource CardBox}">
       <StackPanel>
        <TextBlock x:Name="ConnectHeading" Style="{StaticResource Heading}"/>
        <Grid><Grid.ColumnDefinitions><ColumnDefinition Width="140"/><ColumnDefinition Width="*"/><ColumnDefinition Width="*"/></Grid.ColumnDefinitions>
         <TextBlock x:Name="ApiLabel" Style="{StaticResource FieldLabel}" Margin="0,0,8,0"/><ComboBox x:Name="Api" Grid.Column="1" SelectedIndex="0"><ComboBoxItem x:Name="ApiVulkan" Content=""/><ComboBoxItem x:Name="ApiDxvk" Content=""/></ComboBox>
        </Grid>
        <Expander x:Name="LaunchOptions" Header="" Margin="0,8,0,0">
         <StackPanel Margin="18,2,0,4">
          <TextBlock x:Name="ArgsLabel" Text="" Margin="0,0,0,4"/><TextBox x:Name="GameArgs" ToolTip=""/>
          <CheckBox x:Name="Fossilize" IsChecked="True" Content="" Margin="0,8,0,0" ToolTip=""/>
         </StackPanel>
        </Expander>
        <TextBlock x:Name="FirstTestHint" Style="{StaticResource Note}" Margin="0,8,0,10"/>
        <WrapPanel><Button x:Name="Check"/><Button x:Name="Dependencies"/><Button x:Name="Install" Style="{StaticResource Primary}"/><Button x:Name="Uninstall"/></WrapPanel>
       </StackPanel>
      </Border>
      <Border Style="{StaticResource CardBox}">
       <StackPanel>
        <TextBlock x:Name="CompareHeading" Style="{StaticResource Heading}"/>
        <WrapPanel><Button x:Name="Launch"/><Button x:Name="Toggle"/><Button x:Name="SpeedTest"/></WrapPanel>
        <TextBlock x:Name="EffectState" Text="" FontWeight="SemiBold" Margin="0,4,0,2"/>
        <TextBlock x:Name="RuntimeState" Text="" Style="{StaticResource Note}" FontSize="13"/>
        <TextBlock x:Name="FpsHint" Style="{StaticResource Note}" Margin="0,6,0,0"/>
        <TextBlock x:Name="SpeedState" Style="{StaticResource Note}" Margin="0,6,0,0"/>
       </StackPanel>
      </Border>
      <Border Style="{StaticResource CardBox}">
       <DockPanel>
        <Button x:Name="Report" DockPanel.Dock="Right" VerticalAlignment="Top" Margin="8,0,0,0"/>
        <Expander x:Name="CheckDetails" Header=""><TextBox x:Name="Details" IsReadOnly="True" TextWrapping="Wrap" AcceptsReturn="True" VerticalScrollBarVisibility="Auto" Height="160" FontFamily="Consolas" FontSize="12" VerticalContentAlignment="Top"/></Expander>
       </DockPanel>
      </Border>
     </StackPanel>
    </ScrollViewer>
   </TabItem>
   <TabItem x:Name="ControlsTab" Header="">
    <ScrollViewer VerticalScrollBarVisibility="Auto" HorizontalScrollBarVisibility="Disabled">
     <StackPanel>
      <Border Style="{StaticResource CardBox}">
       <StackPanel>
        <DockPanel>
         <TextBlock x:Name="ControlsEffectState" DockPanel.Dock="Right" FontWeight="SemiBold" VerticalAlignment="Center" Margin="10,0,0,6"/>
         <WrapPanel><Button x:Name="ControlsToggle" Style="{StaticResource Primary}"/><Button x:Name="ControlsLaunch"/></WrapPanel>
        </DockPanel>
        <TextBlock x:Name="ControlsRuntimeState" Style="{StaticResource Note}" FontSize="13"/>
        <TextBlock x:Name="FrameReadout" Style="{StaticResource Note}" Margin="0,2,0,0"/>
       </StackPanel>
      </Border>
      <Border Style="{StaticResource CardBox}">
       <StackPanel>
        <DockPanel Margin="0,0,0,4">
         <Button x:Name="ResetSettings" DockPanel.Dock="Right" Style="{StaticResource Link}"/>
         <TextBlock x:Name="SettingsState" DockPanel.Dock="Right" Style="{StaticResource Note}" VerticalAlignment="Center" Margin="0,0,10,0"/>
         <TextBlock x:Name="ControlsHeading" Style="{StaticResource Heading}" Margin="0" VerticalAlignment="Center"/>
        </DockPanel>
        <StackPanel x:Name="KnobRows"/>
        <Expander x:Name="MoreSettings" Header="" Margin="0,6,0,0"><StackPanel x:Name="MoreKnobRows"/></Expander>
       </StackPanel>
      </Border>
      <Border Style="{StaticResource CardBox}">
       <StackPanel>
        <TextBlock x:Name="PacingHeading" Style="{StaticResource Heading}"/>
        <CheckBox x:Name="AsyncMode" Content="" ToolTip=""/>
        <CheckBox x:Name="GpuImagePath" Content="" ToolTip="" Margin="0,8,0,0"/>
        <TextBlock x:Name="GpuImageNote" Style="{StaticResource Note}" Margin="0,4,0,0"/>
        <Grid Margin="0,8,0,0"><Grid.ColumnDefinitions><ColumnDefinition Width="170"/><ColumnDefinition Width="*"/></Grid.ColumnDefinitions>
         <TextBlock x:Name="EveryLabel" VerticalAlignment="Center" ToolTip=""/>
         <ComboBox x:Name="EveryFrame" Grid.Column="1" Margin="8,0,0,0" SelectedIndex="0" ToolTip=""><ComboBoxItem x:Name="Every1" Tag="1" Content=""/><ComboBoxItem x:Name="Every2" Tag="2" Content=""/><ComboBoxItem x:Name="Every3" Tag="3" Content=""/><ComboBoxItem x:Name="Every4" Tag="4" Content=""/></ComboBox>
        </Grid>
        <TextBlock x:Name="PacingNote" Style="{StaticResource Note}" Margin="0,8,0,0"/>
       </StackPanel>
      </Border>
     </StackPanel>
    </ScrollViewer>
   </TabItem>
  </TabControl>
 </DockPanel>
</Window>
'@
$script:Window = [Windows.Markup.XamlReader]::Load([Xml.XmlNodeReader]::new($xaml))
$script:Ui = @{}
foreach($name in @('DllPath','GamePath','PythonPath','BrowseDll','BrowseGame','BrowsePython','FindPython','GetPython','Api','GameArgs','Fossilize','Check','Dependencies','Install','Uninstall','Launch','Toggle','SpeedTest','EffectState','RuntimeState','Report','Progress','Spinner','Details','CheckDetails','MainScroll','Subtitle','FilesHeading','DllLabel','GameLabel','PythonLabel','ConnectHeading','ApiLabel','ApiVulkan','ApiDxvk','LaunchOptions','ArgsLabel','FirstTestHint','CompareHeading','FpsHint','SpeedState','Language','Theme')) {
    $script:Ui[$name] = $script:Window.FindName($name)
    if($null -eq $script:Ui[$name]) { throw "Missing control $name" }
}
foreach($name in @('Pages','SetupTab','ControlsTab','ControlsHeading','ControlsToggle','ControlsLaunch','ResetSettings','ControlsEffectState','ControlsRuntimeState','FrameReadout','SettingsState','KnobRows','MoreSettings','MoreKnobRows','PacingHeading','AsyncMode','GpuImagePath','GpuImageNote','EveryLabel','EveryFrame','Every1','Every2','Every3','Every4','PacingNote')) {
    $script:Ui[$name]=$script:Window.FindName($name)
    if($null -eq $script:Ui[$name]) { throw "Missing control $name" }
}
$script:Window.Height=[Math]::Min(700,[Windows.SystemParameters]::WorkArea.Height-36)
$script:Window.Width=[Math]::Min(780,[Windows.SystemParameters]::WorkArea.Width-36)

# Light or dark: Windows' own setting for apps, unless the theme button chose one, kept in
# work\windows-wizard\ui.json. Every colour in the window is one of these brushes.
$script:Themes=@{
 light=@{Bg='#F3F5F9';Card='#FFFFFF';CardBorder='#E3E8EF';Text='#1A2433';Muted='#5B6B80';Accent='#2563EB';AccentText='#FFFFFF';Field='#FFFFFF';FieldBorder='#CBD5E1';Hover='#EEF2F7';Selected='#E0E9FB';Track='#D5DCE6';Good='#15803D';Bad='#DC2626'}
 dark=@{Bg='#16191E';Card='#1F2329';CardBorder='#2D333B';Text='#E6E9EE';Muted='#9AA5B4';Accent='#4C8DFF';AccentText='#FFFFFF';Field='#262B32';FieldBorder='#3B424C';Hover='#2B3139';Selected='#233552';Track='#3B424C';Good='#4ADE80';Bad='#F87171'}
}
$script:Theme='light'
$script:UiPrefs=Join-Path $script:Work 'ui.json'
Add-Type -Namespace NrSetup -Name Native -MemberDefinition @'
[DllImport("dwmapi.dll")] public static extern int DwmSetWindowAttribute(IntPtr hwnd, int attribute, ref int value, int size);
[DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hwnd, IntPtr after, int x, int y, int cx, int cy, uint flags);
'@
function Get-SystemTheme {
    $value=(Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize' -Name AppsUseLightTheme -ErrorAction SilentlyContinue).AppsUseLightTheme
    if($value -eq 0) { 'dark' } else { 'light' }
}
function Set-TitleBar {
    $handle=[Windows.Interop.WindowInteropHelper]::new($script:Window).Handle
    if($handle -eq [IntPtr]::Zero) { return }
    $dark=[int]($script:Theme -eq 'dark')
    # 20 is DWMWA_USE_IMMERSIVE_DARK_MODE; Windows 10 before 20H1 knew it as 19.
    if([NrSetup.Native]::DwmSetWindowAttribute($handle,20,[ref]$dark,4) -ne 0) { $null=[NrSetup.Native]::DwmSetWindowAttribute($handle,19,[ref]$dark,4) }
    # NOSIZE|NOMOVE|NOZORDER|NOACTIVATE|FRAMECHANGED: the caption repaints at once.
    $null=[NrSetup.Native]::SetWindowPos($handle,[IntPtr]::Zero,0,0,0,0,0x37)
}
function Set-Theme([string]$Name) {
    $script:Theme=$Name
    foreach($key in $script:Themes[$Name].Keys) {
        $brush=[Windows.Media.SolidColorBrush]::new([Windows.Media.ColorConverter]::ConvertFromString($script:Themes[$Name][$key]))
        $brush.Freeze()
        $script:Window.Resources[$key]=$brush
    }
    # The sun offers the light theme, the moon the dark one.
    $script:Ui.Theme.Content=[string][char]$(if($Name -eq 'dark'){0xE706}else{0xE708})
    Set-TitleBar
}

# Windows UI culture selects the initial language. Switching never edits a profile,
# runtime setting or Steam option; only app-owned labels and messages change.
$script:Language=[Globalization.CultureInfo]::CurrentUICulture.TwoLetterISOLanguageName
if($script:Language -notin @('ru','es','zh')) { $script:Language='en' }
$script:ChangingLanguage=$false
$script:ProgressKey='ChooseFiles'
$script:RuntimeKey='RuntimeInitial'
$script:RuntimeValues=@()
$script:LastStatus=$null
$script:Strings=@{
 en=@{
  Title='DLSS-NR — setup and launch'; LanguageTip='Change the setup language.'
  Subtitle='Setup and test launch on Intel Xe2'; FilesHeading='1. Choose your files'
  DllLabel='Your NVIDIA DLL'; GameLabel='Game executable'; PythonLabel='64-bit Python'; Browse='Browse…'
  DllTip='Choose your own nvngx_dlssnr.dll. The file is not downloaded or included in the package.'
  GameTip='The game''s .exe, 64-bit or 32-bit. For DirectX 8–11, setup puts DXVK beside it.'
  PythonTip='Choose native 64-bit Windows Python, not the MinGW/MSYS2 interpreter.'
  FindPython='Find Python'; GetPython='Python website'
  PythonHint='Python is usually detected automatically. If it is missing, install Windows x64 Python from python.org.'
  ConnectHeading='2. Connect NR to your game'; ApiLabel='Graphics API'; ApiVulkan='Vulkan'; ApiDxvk='DirectX 8–11 (setup adds DXVK)'
  LaunchOptions='Launch options'
  ArgsLabel='Game arguments (optional)'; ArgsTip='Ordinary game arguments; shell commands are not executed here.'
  Fossilize='Steam shader-cache workaround for NR'
  FossilizeTip='Install NR turns off Steam''s Fossilize layer in this game. Steam Overlay remains available.'
  FirstTestHint='For the first test, choose an 800×450 game window; the network starts at scale 0.4. Setup puts its vulkan-1.dll beside the game, and DXVK for DirectX 8–11, setting aside any file it replaces; Remove NR puts everything back. Start the game as you usually do. DirectX 12 games do not work on Windows yet.'
  Check='Check'; Dependencies='Prepare Python'; Install='Install NR'; CompareHeading='3. Launch and compare'
  Launch='Launch game'; Enable='Enable NR'; Disable='Disable NR'
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
  PickDll='Your nvngx_dlssnr.dll'; PickGame='Game executable'; PickPython='64-bit Windows Python'
  DllFilter='NVIDIA DLSS-NR DLL|nvngx_dlssnr.dll|DLL files|*.dll'; GameFilter='Game executable|*.exe'
  PythonFilter='Python interpreter|python.exe;python3.exe|Executable|*.exe'; ReportFilter='JSON report|*.json'
  ReportTitle='Save diagnostic report'
  'Loading.discover'='Looking for suitable Python…'; 'Loading.check'='Checking files and dependencies…'
  'Loading.dependencies'='Preparing local Python…'; 'Loading.install'='Extracting weights and installing NR…'
  'Loading.launch'='Launching the game…'
  'Loading.on'='Enabling NR…'; 'Loading.off'='Disabling NR…'
  'Loading.report'='Saving report…'; 'Loading.save'='Saving settings…'
  'Done.discover'='Python found. Choose your game and DLL.'; 'Done.check'='Checks passed. You can install NR.'
  'Done.dependencies'='Python is ready. Now install NR.'; 'Done.install'='NR installed. Start the game as you usually do, or with Launch game.'
  'Done.launch'='Launch requested. Enter a game scene.'; 'Done.install-steam'='Steam''s launch options from an earlier setup returned, and NR installed. Start the game as you usually do.'
  'Done.on'='NR enabled. Wait for the first processed frames.'
  'Done.off'='NR disabled.'; 'Done.report'='Report saved.'; 'Done.save'='Settings saved.'
  SetupTab='Setup'; ControlsTab='NR controls'; ControlsHeading='Live NR controls'
  ControlsHint='Changes save automatically after you finish adjusting. The daemon reads them between frames; no game restart is needed. Requested scale and the actual padded network size may differ.'
  ResetSettings='Reset all'; DefaultKnob='Default'; MoreSettings='More settings'; ThemeTip='Light or dark theme'
  SettingsLoading='Loading NR settings…'; SettingsReady='Current settings loaded.'; SettingsUnsaved='Changes waiting to be saved…'
  SettingsSaved='Saved. Used on the next processed frame.'; SettingsFailed='Settings could not be saved. See Setup → Check details.'
  UnsavedClose='Some NR changes were not saved. Close and discard those changes?'
  InvalidSetting='Enter a finite number within the supported range.'; FrameReadout='Last neural frame: output {0}×{1}; network {3}×{4}; processing {2} ms. This is not game FPS.'
  PackagesLater='Python works. Install NR adds numpy and safetensors to a Python of its own.'
  GameWithoutNr='The game is running without NR. Restart it; if NR still does not appear, start it with Launch game.'
  GameElevated='The game runs as administrator, and there the Windows Vulkan loader ignores where NR is, so NR cannot reach it. Close the game and start it without administrator rights, and Steam or the launcher that starts it too.'
  GameElevatedUacOff='The game runs as administrator because UAC is off, and there the Windows Vulkan loader ignores where NR is, so NR cannot reach it. NR needs UAC on, as Windows has it by default.'
  'Warn.vulkan_executable'='{0} does not load the Vulkan loader itself, and NR reaches only the .exe chosen here. If the game draws with Vulkan in another .exe, choose that one.'
  'Warn.vulkan_executable_other'='{0} does not load the Vulkan loader itself, and NR reaches only the .exe chosen here. {1} beside it does: if the game draws with Vulkan there, choose it.'
  'Warn.window_elevated'='This window runs as administrator, and so does a game started with Launch game: NR cannot reach a game that runs as administrator. Start the game, and Steam or the launcher that starts it, without administrator rights.'
  'Warn.uac_off'='UAC is off, so Windows runs every program as administrator, and NR cannot reach a game that runs so. NR needs UAC on, as Windows has it by default.'
  SpeedTest='Speed test'; SpeedTestTip='Measures NR alone on this GPU, without a game, at four window sizes: the best it can do here. Save report carries the result.'
  'Loading.speedtest'='Measuring NR on this GPU, without a game…'; 'Done.speedtest'='Speed test saved; Save report carries it.'
  SpeedHead='The network itself on this PC ({0}), per frame:'; SpeedScale='render scale {0}: {1}'
  SpeedCase='{0}×{1} {2} ms (up to {3} fps)'
  SpeedNote='Taking each frame from the game and giving it back takes time too, and the game draws on the same GPU: in a game the frame rate is lower than these.'
  PacingHeading='Frame pacing'; PacingNote='Takes effect when the game starts next.'
  AsyncMode='Asynchronous: the network works while the game draws its next frame'
  AsyncModeTip='More frames a second, and what is on screen is one processed frame behind.'
  GpuImagePath='Experimental: frames remain on the GPU'
  GpuImagePathTip='Frames remain on the GPU. If the GPU path is unavailable or processing fails, the effect is skipped and the game shows its original frame. Requires the game and network on the same Arc, asynchronous off and every frame.'
  GpuImageNote='Takes effect at the next game start. Asynchronous mode or processing fewer than every frame skips the effect in this mode.'
  EveryLabel='Network on'; EveryTip='The frames between repeat the last result: the game runs faster, and the picture changes only on the network''s frames.'
  Every1='every frame'; Every2='every 2nd frame'; Every3='every 3rd frame'; Every4='every 4th frame'
  'Loading.pacing'='Saving frame pacing…'; 'Done.pacing'='Frame pacing saved; it takes effect when the game starts next.'
  'Done.pacing-later'='Frame pacing saved; Install NR gives it to the game.'
  'Loading.settings'='Loading NR settings…'; 'Loading.settings-save'='Saving NR controls…'; 'Loading.settings-reset'='Restoring NR defaults…'
  'Done.settings'='NR settings loaded.'; 'Done.settings-save'='NR settings saved.'; 'Done.settings-reset'='NR defaults restored.'
  'Knob.render_scale'='Render scale'; 'Knob.min_extent'='Minimum network side'; 'Knob.profile'='Profile'; 'Knob.intensity'='Intensity'
  'Knob.detail_strength'='Detail strength'; 'Knob.colour_strength'='Colour strength'; 'Knob.temporal'='Temporal history'
  'Knob.hold'='Hold unchanged pixels'; 'Knob.release'='Release moving pixels'; 'Knob.cut_limit'='Scene-cut threshold'
  'Hint.render_scale'='Fraction of each dimension processed by the network. Lower is cheaper, with coarser detail; minimum padding can limit the reduction.'
  'Hint.min_extent'='Minimum padded network side. 320 matches the vendor; 128 costs less for small frames and changes the surrounding mirrored context.'
  'Hint.profile'='Standard adds texture; natural and cinematic preserve more highlights.'
  'Hint.intensity'='Blend strength: 0 keeps the game image, 1 uses the model result, above 1 exaggerates the change.'
  'Hint.detail_strength'='Strength of fine detail. Zero keeps only broad tone changes; above 1 adds more sharpening.'
  'Hint.colour_strength'='Strength of tone and colour changes. Higher values can reduce saturation; zero preserves the game tone.'
  'Hint.temporal'='How much previous output is fed back. Zero forgets history and draws frames independently.'
  'Hint.hold'='Keep the previous result where the game pixels did not change, reducing flicker in still areas.'
  'Hint.release'='Pixel change in levels of 255 that releases history. 16–24 is a useful starting range; zero disables this protection.'
  'Hint.cut_limit'='Average frame change that discards history for a scene cut. Lower reacts sooner; 1 never cuts.'
  'Knob.weights'='Weights model'; 'Hint.weights'='Which weight model the daemon runs. HT is the model this project reproduces; the small FP8 model newer DLLs carry is detected and extracted, and is not loadable yet.'
  Uninstall='Remove NR'; 'Loading.uninstall'='Removing NR from the game…'; 'Done.uninstall'='NR removed. The game folder is as it was before installation.'
  'Done.uninstall-steam'='Steam''s launch options restored and NR removed. The game folder is as it was before installation.'
  UninstallConfirm='Remove NR from this game? Setup deletes the dlss-nr folder and the files it put beside the game, and puts back the game files it set aside. If Steam still starts the game through an earlier setup''s launch options, they go back first, and Steam restarts. Your weights and settings stay in the DLSS-NR folder.'
  'Profile.standard'='Standard'; 'Profile.natural'='Natural'; 'Profile.cinematic'='Cinematic'
  'Choice.weights.ht'='HT (the model this project reproduces)'; 'Choice.weights.cc_small_fp8'='CC small FP8 (not loadable yet)'
 }
 ru=@{
  Title='DLSS-NR — установка и запуск'; LanguageTip='Выберите язык мастера.'
  Subtitle='Установка и пробный запуск на Intel Xe2'; FilesHeading='1. Выберите файлы'
  DllLabel='Ваша NVIDIA DLL'; GameLabel='Программа игры'; PythonLabel='64-битный Python'; Browse='Обзор…'
  DllTip='Выберите собственную nvngx_dlssnr.dll. Файл не скачивается и не входит в пакет.'
  GameTip='Файл .exe игры, 64- или 32-битный. Для DirectX 8–11 мастер положит рядом DXVK.'
  PythonTip='Выберите обычный 64-битный Windows Python, а не интерпретатор MinGW/MSYS2.'
  FindPython='Найти Python'; GetPython='Сайт Python'
  PythonHint='Python обычно определяется автоматически. Если его нет, установите Windows x64 Python с python.org.'
  ConnectHeading='2. Подключите NR к игре'; ApiLabel='Графический API'; ApiVulkan='Vulkan'; ApiDxvk='DirectX 8–11 (мастер добавит DXVK)'
  LaunchOptions='Параметры запуска'
  ArgsLabel='Аргументы игры (необязательно)'; ArgsTip='Обычные аргументы игры; команды оболочки здесь не выполняются.'
  Fossilize='Обход Steam shader-cache для NR'
  FossilizeTip='«Установить NR» отключит в этой игре слой Fossilize от Steam. Steam Overlay остаётся доступен.'
  FirstTestHint='Для первого теста выберите в игре окно 800×450; сеть начнёт с масштаба 0.4. Мастер положит рядом с игрой свою vulkan-1.dll, а для DirectX 8–11 ещё и DXVK, и отложит файлы, которые заменит; «Удалить NR» всё вернёт. Запускайте игру как обычно. Игры на DirectX 12 в Windows пока не работают.'
  Check='Проверить'; Dependencies='Подготовить Python'; Install='Установить NR'; CompareHeading='3. Запустите и сравните'
  Launch='Запустить игру'; Enable='Включить NR'; Disable='Выключить NR'
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
  PickDll='Ваша nvngx_dlssnr.dll'; PickGame='Программа игры'; PickPython='64-битный Windows Python'
  DllFilter='NVIDIA DLSS-NR DLL|nvngx_dlssnr.dll|Файлы DLL|*.dll'; GameFilter='Программа игры|*.exe'
  PythonFilter='Интерпретатор Python|python.exe;python3.exe|Программа|*.exe'; ReportFilter='Отчёт JSON|*.json'
  ReportTitle='Сохранить диагностический отчёт'
  'Loading.discover'='Ищем подходящий Python…'; 'Loading.check'='Проверяем файлы и зависимости…'
  'Loading.dependencies'='Готовим локальный Python…'; 'Loading.install'='Извлекаем веса и устанавливаем NR…'
  'Loading.launch'='Запускаем игру…'
  'Loading.on'='Включаем NR…'; 'Loading.off'='Выключаем NR…'
  'Loading.report'='Сохраняем отчёт…'; 'Loading.save'='Сохраняем параметры…'
  'Done.discover'='Python найден. Выберите игру и DLL.'; 'Done.check'='Проверка пройдена. Можно установить NR.'
  'Done.dependencies'='Python подготовлен. Теперь установите NR.'; 'Done.install'='NR установлен. Запускайте игру как обычно или кнопкой «Запустить игру».'
  'Done.launch'='Запуск запрошен. Войдите в игровую сцену.'; 'Done.install-steam'='Параметры запуска Steam от прежней версии мастера возвращены, NR установлен. Запускайте игру как обычно.'
  'Done.on'='NR включён. Дождитесь первых обработанных кадров.'
  'Done.off'='NR выключен.'; 'Done.report'='Отчёт сохранён.'; 'Done.save'='Параметры сохранены.'
  SetupTab='Установка'; ControlsTab='Настройки NR'; ControlsHeading='Настройки NR во время игры'
  ControlsHint='Изменения сохраняются автоматически после регулировки. Демон читает их между кадрами; перезапуск игры не нужен. Выбранный масштаб и фактический размер сети с дополнением могут различаться.'
  ResetSettings='Сбросить всё'; DefaultKnob='По умолчанию'; MoreSettings='Дополнительно'; ThemeTip='Светлая или тёмная тема'
  SettingsLoading='Загружаем настройки NR…'; SettingsReady='Текущие настройки загружены.'; SettingsUnsaved='Изменения ожидают сохранения…'
  SettingsSaved='Сохранено. Используется со следующего обработанного кадра.'; SettingsFailed='Не удалось сохранить. См. «Установка» → «Подробности проверки».'
  UnsavedClose='Некоторые изменения NR не сохранены. Закрыть окно и отбросить эти изменения?'
  InvalidSetting='Введите конечное число в допустимом диапазоне.'; FrameReadout='Последний нейронный кадр: выход {0}×{1}; сеть {3}×{4}; обработка {2} мс. Это не FPS игры.'
  PackagesLater='Python подходит. «Установить NR» сам поставит numpy и safetensors в свою копию Python.'
  GameWithoutNr='Игра запущена без NR. Перезапустите её; если NR так и не появится, запустите её кнопкой «Запустить игру».'
  GameElevated='Игра запущена с правами администратора: в таком процессе загрузчик Vulkan в Windows не видит NR, и NR до игры не доходит. Закройте игру и запустите её без прав администратора, а также Steam или лаунчер, который её запускает.'
  GameElevatedUacOff='Игра запущена с правами администратора, потому что UAC выключен: в таком процессе загрузчик Vulkan в Windows не видит NR, и NR до игры не доходит. NR нужен включённый UAC, как в Windows по умолчанию.'
  'Warn.vulkan_executable'='{0} сам не загружает загрузчик Vulkan, а NR работает только в выбранном здесь .exe. Если игра рисует через Vulkan в другом .exe, выберите его.'
  'Warn.vulkan_executable_other'='{0} сам не загружает загрузчик Vulkan, а NR работает только в выбранном здесь .exe. Рядом лежит {1}, который его загружает: если игра рисует через Vulkan там, выберите его.'
  'Warn.window_elevated'='Это окно запущено с правами администратора, и игра, запущенная кнопкой «Запустить игру», тоже: до игры с такими правами NR не доходит. Запускайте игру, а также Steam или лаунчер, который её запускает, без прав администратора.'
  'Warn.uac_off'='UAC выключен, поэтому Windows запускает все программы с правами администратора, и до игры с такими правами NR не доходит. NR нужен включённый UAC, как в Windows по умолчанию.'
  SpeedTest='Тест скорости'; SpeedTestTip='Измеряет NR на этой видеокарте без игры, при четырёх размерах окна: лучшее, на что он здесь способен. Результат попадает в отчёт.'
  'Loading.speedtest'='Измеряем NR на этой видеокарте, без игры…'; 'Done.speedtest'='Тест скорости сохранён и попадёт в отчёт.'
  SpeedHead='Сама нейросеть на этом ПК ({0}) тратит на кадр:'; SpeedScale='масштаб рендера {0}: {1}'
  SpeedCase='{0}×{1} — {2} мс (до {3} fps)'
  SpeedNote='Захват кадра из игры и его возврат тоже занимают время, а игра рисует на той же видеокарте, поэтому fps в игре будет ниже этих значений.'
  PacingHeading='Подача кадров'; PacingNote='Применится при следующем запуске игры.'
  AsyncMode='Асинхронно: сеть работает, пока игра рисует следующий кадр'
  AsyncModeTip='Больше кадров в секунду, а на экране результат на один обработанный кадр позже.'
  GpuImagePath='Экспериментально: кадры остаются на GPU'
  GpuImagePathTip='Кадры остаются на GPU. Если этот путь недоступен или обработка завершилась с ошибкой, эффект пропускается и показывается исходный кадр игры. Нужны игра и сеть на одной Arc, выключенный асинхронный режим и обработка каждого кадра.'
  GpuImageNote='Применится при следующем запуске игры. При асинхронной подаче или обработке реже каждого кадра эффект в этом режиме пропускается.'
  EveryLabel='Сеть на'; EveryTip='Кадры между ними повторяют последний результат: игра идёт быстрее, а картинка меняется только на кадрах сети.'
  Every1='каждом кадре'; Every2='каждом 2-м кадре'; Every3='каждом 3-м кадре'; Every4='каждом 4-м кадре'
  'Loading.pacing'='Сохраняем подачу кадров…'; 'Done.pacing'='Подача кадров сохранена и применится при следующем запуске игры.'
  'Done.pacing-later'='Подача кадров сохранена; «Установить NR» передаст её игре.'
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
  'Knob.weights'='Модель весов'; 'Hint.weights'='Какую модель весов запускает демон. HT — модель, которую воспроизводит проект; малая FP8-модель из новых DLL определяется и извлекается, но её раскладка блоков пока не загружается.'
  Uninstall='Удалить NR'; 'Loading.uninstall'='Удаляем NR из игры…'; 'Done.uninstall'='NR удалён. Папка игры такая же, как до установки.'
  'Done.uninstall-steam'='Параметры запуска Steam возвращены, NR удалён. Папка игры такая же, как до установки.'
  UninstallConfirm='Удалить NR из этой игры? Мастер удалит папку dlss-nr и файлы, которые положил рядом с игрой, и вернёт отложенные файлы игры. Если Steam ещё запускает игру с параметрами от прежней версии мастера, сначала вернутся они, и Steam перезапустится. Веса и настройки останутся в папке DLSS-NR.'
  'Profile.standard'='Стандартный'; 'Profile.natural'='Естественный'; 'Profile.cinematic'='Кинематографический'
  'Choice.weights.ht'='HT (модель, которую воспроизводит проект)'; 'Choice.weights.cc_small_fp8'='CC small FP8 (пока не загружается)'
 }
 es=@{
  Title='DLSS-NR — instalación e inicio'; LanguageTip='Cambie el idioma del instalador.'
  Subtitle='Instalación y prueba en Intel Xe2'; FilesHeading='1. Elija sus archivos'
  DllLabel='Su DLL de NVIDIA'; GameLabel='Ejecutable del juego'; PythonLabel='Python de 64 bits'; Browse='Examinar…'
  DllTip='Elija su propio nvngx_dlssnr.dll. El archivo no se descarga ni se incluye en el paquete.'
  GameTip='El .exe del juego, de 64 o 32 bits. Para DirectX 8–11, el instalador pone DXVK a su lado.'
  PythonTip='Elija Python nativo de Windows de 64 bits, no el intérprete de MinGW/MSYS2.'
  FindPython='Buscar Python'; GetPython='Sitio web de Python'
  PythonHint='Python suele detectarse automáticamente. Si falta, instale Python para Windows x64 desde python.org.'
  ConnectHeading='2. Conecte NR a su juego'; ApiLabel='API gráfica'; ApiVulkan='Vulkan'; ApiDxvk='DirectX 8–11 (el instalador añade DXVK)'
  LaunchOptions='Opciones de inicio'
  ArgsLabel='Argumentos del juego (opcional)'; ArgsTip='Argumentos normales del juego; aquí no se ejecutan comandos de la consola.'
  Fossilize='Solución para la caché de sombreadores de Steam con NR'
  FossilizeTip='Instalar NR desactiva la capa Fossilize de Steam en este juego. El Steam Overlay sigue disponible.'
  FirstTestHint='Para la primera prueba, elija en el juego una ventana de 800×450; la red empieza con escala 0.4. El instalador pone su vulkan-1.dll junto al juego, y DXVK para DirectX 8–11, y guarda aparte los archivos que reemplaza; Quitar NR lo devuelve todo. Inicie el juego como siempre. Los juegos DirectX 12 todavía no funcionan en Windows.'
  Check='Comprobar'; Dependencies='Preparar Python'; Install='Instalar NR'; CompareHeading='3. Inicie y compare'
  Launch='Iniciar el juego'; Enable='Activar NR'; Disable='Desactivar NR'
  EffectOn='Efecto activado'; EffectOff='Efecto desactivado'
  RuntimeInitial='El juego aún no se ha probado. Después de activar NR, espere a los fotogramas procesados.'
  FpsHint='NR añade procesamiento a cada fotograma y puede bajar mucho los FPS. Compare la misma escena con el efecto activado y desactivado.'
  Report='Guardar informe'; CheckDetails='Detalles de la comprobación'; ChooseFiles='Elija su juego y su propia DLL.'
  PythonMissing='No se encontró Python. Instale Python para Windows x64 o elija python.exe.'
  ActionFailed='No se pudo completar la acción.'; NoResult='La acción terminó sin resultado. Abra los detalles.'
  ResultMissing='No se generó ningún resultado.'; FixChecks='Corrija los puntos señalados. Abra los detalles de la comprobación.'
  StatusUnavailable='El estado no está disponible ahora. Se mantiene el último estado conocido del efecto.'
  Network='Red: {0}×{1}.'; Frames='Fotogramas'; TailFrames='Fotogramas en la última parte del registro'
  Processing='El juego se está procesando. {0}: {1}; rechazados: {2}. {3}'
  Historical='El registro contiene {0} fotogramas procesados; rechazados: {1}. No hay fotogramas recientes confirmados.'
  WaitingFrames='Todavía no hay fotogramas procesados recientes. Entre en una escena del juego y active NR.'
  BusyClose='Espere a que termine la acción en curso.'
  PickDll='Su nvngx_dlssnr.dll'; PickGame='Ejecutable del juego'; PickPython='Python de Windows de 64 bits'
  DllFilter='DLL de NVIDIA DLSS-NR|nvngx_dlssnr.dll|Archivos DLL|*.dll'; GameFilter='Ejecutable del juego|*.exe'
  PythonFilter='Intérprete de Python|python.exe;python3.exe|Ejecutable|*.exe'; ReportFilter='Informe JSON|*.json'
  ReportTitle='Guardar el informe de diagnóstico'
  'Loading.discover'='Buscando un Python adecuado…'; 'Loading.check'='Comprobando archivos y dependencias…'
  'Loading.dependencies'='Preparando el Python local…'; 'Loading.install'='Extrayendo los pesos e instalando NR…'
  'Loading.launch'='Iniciando el juego…'
  'Loading.on'='Activando NR…'; 'Loading.off'='Desactivando NR…'
  'Loading.report'='Guardando el informe…'; 'Loading.save'='Guardando la configuración…'
  'Done.discover'='Python encontrado. Elija su juego y su DLL.'; 'Done.check'='Comprobación superada. Ya puede instalar NR.'
  'Done.dependencies'='Python está listo. Ahora instale NR.'; 'Done.install'='NR instalado. Inicie el juego como siempre, o con Iniciar el juego.'
  'Done.launch'='Inicio solicitado. Entre en una escena del juego.'; 'Done.install-steam'='Se restauraron las opciones de inicio de Steam de un instalador anterior y se instaló NR. Inicie el juego como siempre.'
  'Done.on'='NR activado. Espere a los primeros fotogramas procesados.'
  'Done.off'='NR desactivado.'; 'Done.report'='Informe guardado.'; 'Done.save'='Configuración guardada.'
  SetupTab='Instalación'; ControlsTab='Controles de NR'; ControlsHeading='Controles de NR durante el juego'
  ControlsHint='Los cambios se guardan solos al terminar de ajustar. El proceso de NR los lee entre fotogramas; no hace falta reiniciar el juego. La escala elegida y el tamaño real de la red con relleno pueden diferir.'
  ResetSettings='Restablecer todo'; DefaultKnob='Por defecto'; MoreSettings='Más ajustes'; ThemeTip='Tema claro u oscuro'
  SettingsLoading='Cargando la configuración de NR…'; SettingsReady='Configuración actual cargada.'; SettingsUnsaved='Cambios pendientes de guardar…'
  SettingsSaved='Guardado. Se usará en el siguiente fotograma procesado.'; SettingsFailed='No se pudo guardar la configuración. Vea Instalación → Detalles de la comprobación.'
  UnsavedClose='Algunos cambios de NR no se guardaron. ¿Cerrar y descartar esos cambios?'
  InvalidSetting='Escriba un número finito dentro del rango admitido.'; FrameReadout='Último fotograma neuronal: salida {0}×{1}; red {3}×{4}; procesamiento {2} ms. Esto no son los FPS del juego.'
  PackagesLater='Python sirve. Instalar NR añade numpy y safetensors a una copia propia de Python.'
  GameWithoutNr='El juego se está ejecutando sin NR. Reinícielo; si NR sigue sin aparecer, inícielo con Iniciar el juego.'
  GameElevated='El juego se ejecuta como administrador, y ahí el cargador de Vulkan de Windows ignora dónde está NR, así que NR no llega a él. Cierre el juego e inícielo sin derechos de administrador, y también Steam o el lanzador que lo inicia.'
  GameElevatedUacOff='El juego se ejecuta como administrador porque el UAC está desactivado, y ahí el cargador de Vulkan de Windows ignora dónde está NR, así que NR no llega a él. NR necesita el UAC activado, como viene en Windows.'
  'Warn.vulkan_executable'='{0} no carga por sí mismo el cargador de Vulkan, y NR solo llega al .exe elegido aquí. Si el juego dibuja con Vulkan en otro .exe, elija ese.'
  'Warn.vulkan_executable_other'='{0} no carga por sí mismo el cargador de Vulkan, y NR solo llega al .exe elegido aquí. {1}, a su lado, sí lo carga: si el juego dibuja con Vulkan allí, elíjalo.'
  'Warn.window_elevated'='Esta ventana se ejecuta como administrador, y también un juego iniciado con Iniciar el juego: NR no llega a un juego que se ejecuta como administrador. Inicie el juego, y Steam o el lanzador que lo inicia, sin derechos de administrador.'
  'Warn.uac_off'='El UAC está desactivado, así que Windows ejecuta todos los programas como administrador, y NR no llega a un juego que se ejecuta así. NR necesita el UAC activado, como viene en Windows.'
  SpeedTest='Prueba de velocidad'; SpeedTestTip='Mide NR solo en esta GPU, sin juego, con cuatro tamaños de ventana: lo mejor que puede hacer aquí. El informe incluye el resultado.'
  'Loading.speedtest'='Midiendo NR en esta GPU, sin juego…'; 'Done.speedtest'='Prueba de velocidad guardada; el informe la incluye.'
  SpeedHead='La red neuronal por sí sola en este PC ({0}), por fotograma:'; SpeedScale='escala de renderizado {0}: {1}'
  SpeedCase='{0}×{1} {2} ms (hasta {3} fps)'
  SpeedNote='Tomar cada fotograma del juego y devolverlo también lleva tiempo, y el juego dibuja en la misma GPU: en un juego los FPS son más bajos que estos.'
  PacingHeading='Ritmo de fotogramas'; PacingNote='Se aplica la próxima vez que se inicie el juego.'
  AsyncMode='Asíncrono: la red trabaja mientras el juego dibuja el siguiente fotograma'
  AsyncModeTip='Más fotogramas por segundo, y lo que se ve va un fotograma procesado por detrás.'
  GpuImagePath='Experimental: los fotogramas permanecen en la GPU'
  GpuImagePathTip='Los fotogramas permanecen en la GPU. Si esta ruta no está disponible o falla el procesamiento, se omite el efecto y se muestra el fotograma original del juego. Requiere el juego y la red en la misma Arc, modo asíncrono desactivado y todos los fotogramas.'
  GpuImageNote='Se aplica al próximo inicio del juego. En este modo se omite el efecto si se activa el modo asíncrono o se procesa menos de todos los fotogramas.'
  EveryLabel='Red en'; EveryTip='Los fotogramas intermedios repiten el último resultado: el juego va más rápido, y la imagen cambia solo en los fotogramas de la red.'
  Every1='cada fotograma'; Every2='uno de cada 2 fotogramas'; Every3='uno de cada 3 fotogramas'; Every4='uno de cada 4 fotogramas'
  'Loading.pacing'='Guardando el ritmo de fotogramas…'; 'Done.pacing'='Ritmo de fotogramas guardado; se aplica la próxima vez que se inicie el juego.'
  'Done.pacing-later'='Ritmo de fotogramas guardado; Instalar NR se lo pasa al juego.'
  'Loading.settings'='Cargando la configuración de NR…'; 'Loading.settings-save'='Guardando los controles de NR…'; 'Loading.settings-reset'='Restaurando los valores predeterminados de NR…'
  'Done.settings'='Configuración de NR cargada.'; 'Done.settings-save'='Configuración de NR guardada.'; 'Done.settings-reset'='Se restauraron los valores predeterminados de NR.'
  'Knob.render_scale'='Escala de renderizado'; 'Knob.min_extent'='Lado mínimo de la red'; 'Knob.profile'='Perfil'; 'Knob.intensity'='Intensidad'
  'Knob.detail_strength'='Fuerza del detalle'; 'Knob.colour_strength'='Fuerza del color'; 'Knob.temporal'='Historial temporal'
  'Knob.hold'='Mantener píxeles sin cambios'; 'Knob.release'='Liberar píxeles en movimiento'; 'Knob.cut_limit'='Umbral de cambio de escena'
  'Hint.render_scale'='Fracción de cada dimensión que procesa la red. Menos es más barato, con detalle más grueso; el relleno mínimo puede limitar la reducción.'
  'Hint.min_extent'='Lado mínimo de la red con relleno. 320 coincide con NVIDIA; 128 cuesta menos en fotogramas pequeños y cambia el contexto reflejado alrededor.'
  'Hint.profile'='Estándar añade textura; natural y cinematográfico conservan mejor las luces; neutro reduce mucho el efecto.'
  'Hint.intensity'='Fuerza de la mezcla: 0 deja la imagen del juego, 1 usa el resultado del modelo y más de 1 exagera el cambio.'
  'Hint.detail_strength'='Fuerza del detalle fino. Cero deja solo los cambios generales de tono; más de 1 añade más nitidez.'
  'Hint.colour_strength'='Fuerza de los cambios de tono y color. Los valores altos pueden bajar la saturación; cero conserva el tono del juego.'
  'Hint.temporal'='Cuánto del resultado anterior vuelve a usarse. Cero olvida el historial y procesa cada fotograma por separado.'
  'Hint.hold'='Mantiene el resultado anterior donde los píxeles del juego no cambiaron, lo que reduce el parpadeo en las zonas quietas.'
  'Hint.release'='Cambio de un píxel, en niveles de 255, que libera el historial. 16–24 es un buen punto de partida; cero desactiva esta protección.'
  'Hint.cut_limit'='Cambio medio del fotograma que descarta el historial en un cambio de escena. Menos reacciona antes; 1 nunca corta.'
  'Knob.weights'='Modelo de pesos'; 'Hint.weights'='Qué modelo de pesos ejecuta el demonio. HT es el modelo que reproduce este proyecto; el modelo FP8 pequeño de los DLL nuevos se detecta y se extrae, y aún no se puede cargar.'
  Uninstall='Quitar NR'; 'Loading.uninstall'='Quitando NR del juego…'; 'Done.uninstall'='NR quitado. La carpeta del juego está como antes de la instalación.'
  'Done.uninstall-steam'='Se restauraron las opciones de inicio de Steam y se quitó NR. La carpeta del juego está como antes de la instalación.'
  UninstallConfirm='¿Quitar NR de este juego? El instalador borra la carpeta dlss-nr y los archivos que puso junto al juego, y devuelve los archivos del juego que guardó aparte. Si Steam todavía inicia el juego con las opciones de un instalador anterior, primero se restauran, y Steam se reinicia. Sus pesos y su configuración se quedan en la carpeta de DLSS-NR.'
  'Profile.standard'='Estándar'; 'Profile.natural'='Natural'; 'Profile.cinematic'='Cinematográfico'
  'Choice.weights.ht'='HT (el modelo que reproduce el proyecto)'; 'Choice.weights.cc_small_fp8'='CC small FP8 (aún no se puede cargar)'
 }
 zh=@{
   Title='DLSS-NR — 安装与启动'; LanguageTip='更改安装程序语言。'
   Subtitle='在 Intel Xe2 上安装并试运行'; FilesHeading='1. 选择文件'
   DllLabel='你的 NVIDIA DLL'; GameLabel='游戏程序'; PythonLabel='64 位 Python'; Browse='浏览…'
   DllTip='选择你自己的 nvngx_dlssnr.dll。该文件不随安装包提供，也不会被下载。'
   GameTip='游戏的 .exe，64 位或 32 位均可。对于 DirectX 8–11，安装程序会同时放入 DXVK。'
   PythonTip='请选择原生 64 位 Windows Python，而不是 MinGW/MSYS2 的解释器。'
   FindPython='查找 Python'; GetPython='Python 官网'
   PythonHint='Python 通常会自动检测到。如果缺少，请从 python.org 安装 Windows x64 版 Python。'
   ConnectHeading='2. 将 NR 连接到游戏'; ApiLabel='图形 API'; ApiVulkan='Vulkan'; ApiDxvk='DirectX 8–11（安装程序会添加 DXVK）'
   LaunchOptions='启动选项'
   ArgsLabel='游戏参数（可选）'; ArgsTip='普通的游戏参数；此处不执行 shell 命令。'
   Fossilize='针对 NR 的 Steam 着色器缓存规避方案'
   FossilizeTip='安装 NR 会在本游戏中关闭 Steam 的 Fossilize 层。Steam 覆盖界面仍然可用。'
   FirstTestHint='首次测试请选择 800×450 的游戏窗口；网络以 0.4 的比例开始运行。安装程序会把它的 vulkan-1.dll 放到游戏旁，并为 DirectX 8–11 放入 DXVK，同时把被替换的文件收起来；移除 NR 会把一切还原。像平常一样启动游戏即可。DirectX 12 游戏目前无法在 Windows 上运行。'
   Check='检查'; Dependencies='准备 Python'; Install='安装 NR'; CompareHeading='3. 启动并对比'
   Launch='启动游戏'; Enable='启用 NR'; Disable='禁用 NR'
   EffectOn='效果已启用'; EffectOff='效果已禁用'
   RuntimeInitial='游戏尚未测试。启用 NR 后，请等待处理完成的帧。'
   FpsHint='NR 会处理每一帧，可能明显降低 FPS。请在效果启用与禁用时对比同一场景。'
   Report='保存报告'; CheckDetails='检查详情'; ChooseFiles='请选择你的游戏和 DLL。'
   PythonMissing='未找到 Python。请安装 Windows x64 版 Python 或选择 python.exe。'
   ActionFailed='操作无法完成。'; NoResult='操作结束了，但没有结果。请打开详情。'
   ResultMissing='没有产生结果。'; FixChecks='请修正报告中指出的问题。请打开检查详情。'
   StatusUnavailable='当前无法获取状态。已保留上一次已知的效果状态。'
   Network='网络：{0}×{1}。'; Frames='帧数'; TailFrames='日志最新段落中的帧数'
   Processing='游戏正在处理。{0}：{1}；已拒绝：{2}。{3}'
   Historical='日志中包含 {0} 个已处理帧；已拒绝：{1}。目前尚未确认有新的帧。'
   WaitingFrames='尚未有新的已处理帧。请进入游戏场景并启用 NR。'
   BusyClose='请等待当前操作完成。'
   PickDll='你的 nvngx_dlssnr.dll'; PickGame='游戏程序'; PickPython='64 位 Windows Python'
   DllFilter='NVIDIA DLSS-NR DLL|nvngx_dlssnr.dll|DLL 文件|*.dll'; GameFilter='游戏程序|*.exe'
   PythonFilter='Python 解释器|python.exe;python3.exe|可执行文件|*.exe'; ReportFilter='JSON 报告|*.json'
   ReportTitle='保存诊断报告'
   'Loading.discover'='正在查找合适的 Python…'; 'Loading.check'='正在检查文件和依赖…'
   'Loading.dependencies'='正在准备本地 Python…'; 'Loading.install'='正在解出权重并安装 NR…'
   'Loading.launch'='正在启动游戏…'
   'Loading.on'='正在启用 NR…'; 'Loading.off'='正在禁用 NR…'
   'Loading.report'='正在保存报告…'; 'Loading.save'='正在保存设置…'
   'Done.discover'='已找到 Python。请选择你的游戏和 DLL。'; 'Done.check'='检查通过。可以安装 NR 了。'
   'Done.dependencies'='Python 已就绪。现在可以安装 NR。'; 'Done.install'='NR 已安装。像平常一样启动游戏，或使用“启动游戏”。'
   'Done.launch'='已请求启动。请进入游戏场景。'; 'Done.install-steam'='已恢复此前安装留下的 Steam 启动选项，并装好了 NR。像平常一样启动游戏即可。'
   'Done.on'='NR 已启用。请等待前几个处理完成的帧。'
   'Done.off'='NR 已禁用。'; 'Done.report'='报告已保存。'; 'Done.save'='设置已保存。'
   SetupTab='安装'; ControlsTab='NR 控制'; ControlsHeading='实时 NR 控制'
   ControlsHint='调整完成后会自动保存。守护进程会在帧之间读取，无需重启游戏。请求的比例与实际补齐的网络尺寸可能不同。'
   ResetSettings='全部重置'; DefaultKnob='默认'; MoreSettings='更多设置'; ThemeTip='浅色或深色主题'
   SettingsLoading='正在加载 NR 设置…'; SettingsReady='当前设置已加载。'; SettingsUnsaved='有更改等待保存…'
   SettingsSaved='已保存。在下一个处理帧上生效。'; SettingsFailed='设置无法保存。请查看“安装 → 检查详情”。'
   UnsavedClose='有部分 NR 更改尚未保存。关闭并放弃这些更改吗？'
   InvalidSetting='请输入支持范围内的有限数值。'; FrameReadout='上一个神经帧：输出 {0}×{1}；网络 {3}×{4}；处理 {2} 毫秒。这不是游戏 FPS。'
   PackagesLater='Python 可用。安装 NR 会向独立的 Python 添加 numpy 和 safetensors。'
   GameWithoutNr='游戏正在没有 NR 的情况下运行。请重启它；如果 NR 仍未出现，请用“启动游戏”启动。'
   GameElevated='游戏以管理员身份运行，此时 Windows 的 Vulkan 加载器会忽略 NR 的位置，因此 NR 无法接入。请关闭游戏，以非管理员身份重新启动，Steam 或启动它的启动器也要如此。'
   GameElevatedUacOff='由于 UAC 已关闭，游戏以管理员身份运行，此时 Windows 的 Vulkan 加载器会忽略 NR 的位置，因此 NR 无法接入。NR 需要 UAC 开启，即 Windows 的默认状态。'
   'Warn.vulkan_executable'='{0} 本身不加载 Vulkan 加载器，NR 只能接入此处选择的 .exe。如果游戏是在另一个 .exe 中以 Vulkan 绘制的，请改选那个。'
   'Warn.vulkan_executable_other'='{0} 本身不加载 Vulkan 加载器，NR 只能接入此处选择的 .exe。它旁边的 {1} 会：如果游戏是以 Vulkan 在那里绘制的，请改选它。'
   'Warn.window_elevated'='本窗口以管理员身份运行，用“启动游戏”启动的游戏也会如此：NR 无法接入以管理员身份运行的游戏。请以非管理员身份启动游戏，以及启动它的 Steam 或启动器。'
   'Warn.uac_off'='UAC 已关闭，Windows 会以管理员身份运行每个程序，NR 无法接入这样运行的游戏。NR 需要 UAC 开启，即 Windows 的默认状态。'
   SpeedTest='速度测试'; SpeedTestTip='在有游戏之前，单独在这块 GPU 上测量 NR，覆盖四种窗口尺寸：这里能达到的最好水平。保存报告会带上该结果。'
   'Loading.speedtest'='正在这块 GPU 上无游戏地测量 NR…'; 'Done.speedtest'='速度测试已保存；保存报告会带上它。'
   SpeedHead='本机（{0}）上网络本身的每帧耗时：'; SpeedScale='渲染比例 {0}：{1}'
   SpeedCase='{0}×{1} {2} 毫秒（最高 {3} fps）'
   SpeedNote='从游戏取每一帧再交回去也要花时间，而且游戏在同一块 GPU 上绘制：在游戏中的帧率会低于这些数字。'
   PacingHeading='帧节奏'; PacingNote='在游戏下次启动时生效。'
   AsyncMode='异步：网络在游戏绘制下一帧时工作'
   AsyncModeTip='每秒帧数更多，而屏幕上显示的比处理结果落后一帧。'
   GpuImagePath='实验性：帧保留在 GPU 上'
   GpuImagePathTip='帧保留在 GPU 上。如果 GPU 路径不可用或处理失败，则跳过效果，游戏显示原始帧。要求游戏与网络在同一块 Arc 上，异步关闭，且每帧都处理。'
   GpuImageNote='在下次游戏启动时生效。异步模式或非每帧处理会在该模式下跳过效果。'
   EveryLabel='网络运行于'; EveryTip='其间各帧重复上一次结果：游戏跑得更快，画面只在网络出帧时变化。'
   Every1='每帧'; Every2='每 2 帧'; Every3='每 3 帧'; Every4='每 4 帧'
   'Loading.pacing'='正在保存帧节奏…'; 'Done.pacing'='帧节奏已保存；在游戏下次启动时生效。'
   'Done.pacing-later'='帧节奏已保存；安装 NR 会把它交给游戏。'
   'Loading.settings'='正在加载 NR 设置…'; 'Loading.settings-save'='正在保存 NR 控制…'; 'Loading.settings-reset'='正在恢复 NR 默认值…'
   'Done.settings'='NR 设置已加载。'; 'Done.settings-save'='NR 设置已保存。'; 'Done.settings-reset'='NR 默认值已恢复。'
   'Knob.render_scale'='渲染比例'; 'Knob.min_extent'='最小网络边长'; 'Knob.profile'='配置风格'; 'Knob.intensity'='强度'
   'Knob.detail_strength'='细节强度'; 'Knob.colour_strength'='色彩强度'; 'Knob.temporal'='时域历史'
   'Knob.hold'='保持未变像素'; 'Knob.release'='释放运动像素'; 'Knob.cut_limit'='转场阈值'
   'Hint.render_scale'='网络处理的每个维度的比例。越低越省，细节也更粗；最小补齐可能限制降低幅度。'
   'Hint.min_extent'='补齐后的最小网络边长。320 与原厂一致；128 对小帧更省，但会改变周围镜像的上下文。'
   'Hint.profile'='标准增加纹理；自然与电影保留更多高光。'
   'Hint.intensity'='混合强度：0 保留游戏画面，1 使用模型结果，大于 1 会夸大变化。'
   'Hint.detail_strength'='细节强度。零只保留大体的色调变化；大于 1 增加更多锐化。'
   'Hint.colour_strength'='色调与色彩变化的强度。更高会降低饱和度；零保留游戏色调。'
   'Hint.temporal'='回馈多少上一次的输出。零则忘记历史，逐帧独立绘制。'
   'Hint.hold'='在游戏像素未变处保留上一次结果，减少静止区域的闪烁。'
   'Hint.release'='以 255 级表示的像素变化量，达到则释放历史。16–24 是有用的起始范围；零则关闭该保护。'
   'Hint.cut_limit'='平均帧变化量，达到则因转场丢弃历史。越低反应越快；1 从不切换。'
   'Knob.weights'='权重模型'; 'Hint.weights'='守护进程运行哪个权重模型。HT 是本项目复现的模型；新版 DLL 里的小型 FP8 模型会被识别并提取，其分块结构目前还不能加载。'
   Uninstall='移除 NR'; 'Loading.uninstall'='正在从游戏移除 NR…'; 'Done.uninstall'='NR 已移除。游戏文件夹恢复如初。'
   'Done.uninstall-steam'='Steam 启动选项已恢复，NR 已移除。游戏文件夹恢复如初。'
   UninstallConfirm='要从本游戏移除 NR 吗？安装程序会删除 dlss-nr 文件夹和它放到游戏旁的文件，并还原它收起来的游戏文件。如果 Steam 仍通过此前安装的启动选项启动游戏，它们会先被恢复，然后 Steam 重启。你的权重和设置保留在 DLSS-NR 文件夹中。'
   'Profile.standard'='标准'; 'Profile.natural'='自然'; 'Profile.cinematic'='电影'
   'Choice.weights.ht'='HT（本项目复现的模型）'; 'Choice.weights.cc_small_fp8'='CC small FP8（暂不可加载）'
  }
}
$script:TextBindings=@{
 Subtitle='Subtitle'; FilesHeading='FilesHeading'; DllLabel='DllLabel'; GameLabel='GameLabel'; PythonLabel='PythonLabel'
 ConnectHeading='ConnectHeading'; ApiLabel='ApiLabel'
 ArgsLabel='ArgsLabel'; FirstTestHint='FirstTestHint'; CompareHeading='CompareHeading'
 FpsHint='FpsHint'; ControlsHeading='ControlsHeading'
 PacingHeading='PacingHeading'; EveryLabel='EveryLabel'; PacingNote='PacingNote'; GpuImageNote='GpuImageNote'
}
$script:ContentBindings=@{
 BrowseDll='Browse'; BrowseGame='Browse'; BrowsePython='Browse'; FindPython='FindPython'; GetPython='GetPython'
 ApiVulkan='ApiVulkan'; ApiDxvk='ApiDxvk'; Fossilize='Fossilize'
 Check='Check'; Dependencies='Dependencies'; Install='Install'; Uninstall='Uninstall'; Launch='Launch'
 Report='Report'; ControlsLaunch='Launch'; ResetSettings='ResetSettings'; SpeedTest='SpeedTest'
 AsyncMode='AsyncMode'; GpuImagePath='GpuImagePath'; Every1='Every1'; Every2='Every2'; Every3='Every3'; Every4='Every4'
}
$script:HeaderBindings=@{LaunchOptions='LaunchOptions'; CheckDetails='CheckDetails';SetupTab='SetupTab';ControlsTab='ControlsTab';MoreSettings='MoreSettings'}
$script:TipBindings=@{DllPath='DllTip'; GamePath='GameTip'; PythonPath='PythonTip'; GameArgs='ArgsTip'; Fossilize='FossilizeTip'; Language='LanguageTip'
 PythonLabel='PythonHint'; ControlsHeading='ControlsHint'; Theme='ThemeTip'; SpeedTest='SpeedTestTip'
 AsyncMode='AsyncModeTip'; GpuImagePath='GpuImagePathTip'; EveryLabel='EveryTip'; EveryFrame='EveryTip'}
function T([string]$Key) {
    if(-not $script:Strings[$script:Language].ContainsKey($Key)) { throw "Missing translation: $Key" }
    return [string]$script:Strings[$script:Language][$Key]
}
# The label of one value of a `choice` knob. A choice knob's values are not always profiles: the
# weight model's are `ht` and `cc_small_fp8`, so the general key is `Choice.<knob>.<value>` and
# `Profile.<value>` stays what `profile` reads. A value with neither key is shown as it is spelled,
# because a missing translation throws above and a settings page that will not open is worse than a
# row that says `ht`.
function ChoiceLabel([string]$Name,[string]$Value) {
    $key='Choice.'+$Name+'.'+$Value
    if($script:Strings[$script:Language].ContainsKey($key)) { return [string]$script:Strings[$script:Language][$key] }
    $key='Profile.'+$Value
    if($script:Strings[$script:Language].ContainsKey($key)) { return [string]$script:Strings[$script:Language][$key] }
    return $Value
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
            $row.Input.SetResourceReference([Windows.Controls.Control]::BorderBrushProperty,'FieldBorder')
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
        $InputControl.SetResourceReference([Windows.Controls.Control]::BorderBrushProperty,'Bad')
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
        $row.Label.Text=T ('Knob.'+$name)
        # The description once, on the name: it was printed under the slider and was the
        # slider's tooltip as well.
        $row.Label.ToolTip=T ('Hint.'+$name)
        $defaultLabel=$(if($row.Model.kind -eq 'choice'){ChoiceLabel $name ([string]$row.Model.default)}else{Format-KnobValue $row.Model.default})
        $row.Default.ToolTip=(T 'DefaultKnob')+': '+$defaultLabel
        [Windows.Automation.AutomationProperties]::SetName($row.Default,(T 'DefaultKnob')+' '+$defaultLabel)
        if($row.Choice) {
            [Windows.Automation.AutomationProperties]::SetName($row.Choice,(T ('Knob.'+$name)))
            foreach($item in $row.Choice.Items) { $item.Content=ChoiceLabel $name ([string]$item.Tag) }
        } else {
            $row.Input.ToolTip=(T ('Knob.'+$name))+' ['+$row.Model.runtime_low+' – '+$row.Model.runtime_high+']'
            [Windows.Automation.AutomationProperties]::SetName($row.Input,(T ('Knob.'+$name)))
            [Windows.Automation.AutomationProperties]::SetName($row.Slider,(T ('Knob.'+$name)))
        }
    }
}
# The three a first game needs; the other seven wait under More settings.
$script:MainKnobs=@('render_scale','profile','intensity')
function Create-KnobControls($Schema) {
    $script:KnobUi=@{}; $script:Ui.KnobRows.Children.Clear(); $script:Ui.MoreKnobRows.Children.Clear()
    $script:LoadingSettings=$true
    try {
        foreach($model in $Schema.knobs) {
            $name=[string]$model.name
            $grid=[Windows.Controls.Grid]::new(); $grid.Margin=[Windows.Thickness]::new(0,2,0,2)
            foreach($width in @('170','*','62','30')) { $column=[Windows.Controls.ColumnDefinition]::new(); $column.Width=[Windows.GridLengthConverter]::new().ConvertFromString($width); $null=$grid.ColumnDefinitions.Add($column) }
            $label=[Windows.Controls.TextBlock]::new(); $label.VerticalAlignment='Center'; $label.TextTrimming='CharacterEllipsis'; $null=$grid.Children.Add($label)
            $reset=[Windows.Controls.Button]::new(); $reset.Tag=$name; $reset.Style=$script:Window.FindResource('Icon'); $reset.Content=[string][char]0xE7A7; $reset.Margin=[Windows.Thickness]::new(2,0,0,0); [Windows.Controls.Grid]::SetColumn($reset,3); $null=$grid.Children.Add($reset)
            [Windows.Automation.AutomationProperties]::SetAutomationId($reset,'NrReset_'+$name)
            $reset.Add_Click({param($sender,$eventArgs); Reset-NrSettings @([string]$sender.Tag)})
            $row=@{Model=$model;Label=$label;Default=$reset;Input=$null;Slider=$null;Choice=$null}
            if($model.kind -eq 'choice') {
                $choice=[Windows.Controls.ComboBox]::new(); $choice.Tag=$name; $choice.Margin=[Windows.Thickness]::new(8,0,0,0)
                foreach($value in $model.choices) { $item=[Windows.Controls.ComboBoxItem]::new(); $item.Tag=[string]$value; $null=$choice.Items.Add($item) }
                [Windows.Controls.Grid]::SetColumn($choice,1); [Windows.Controls.Grid]::SetColumnSpan($choice,2); $null=$grid.Children.Add($choice); $row.Choice=$choice
                [Windows.Automation.AutomationProperties]::SetAutomationId($choice,'NrChoice_'+$name)
                $choice.Add_SelectionChanged({param($sender,$eventArgs); if($sender.SelectedItem) { Change-Knob ([string]$sender.Tag) ([string]$sender.SelectedItem.Tag) }})
            } else {
                $slider=[Windows.Controls.Slider]::new(); $slider.Tag=$name; $slider.Minimum=[double]$model.low; $slider.Maximum=[double]$model.high; $slider.TickFrequency=[double]$model.step; $slider.SmallChange=[double]$model.step; $slider.LargeChange=[double]$model.step*5; $slider.IsSnapToTickEnabled=$true; $slider.VerticalAlignment='Center'; $slider.Margin=[Windows.Thickness]::new(8,0,10,0)
                [Windows.Controls.Grid]::SetColumn($slider,1); $null=$grid.Children.Add($slider); $row.Slider=$slider
                [Windows.Automation.AutomationProperties]::SetAutomationId($slider,'NrSlider_'+$name)
                $inputBox=[Windows.Controls.TextBox]::new(); $inputBox.Tag=$name; $inputBox.Padding=[Windows.Thickness]::new(4,3,4,3); $inputBox.HorizontalContentAlignment='Right'; [Windows.Controls.Grid]::SetColumn($inputBox,2); $null=$grid.Children.Add($inputBox); $row.Input=$inputBox
                [Windows.Automation.AutomationProperties]::SetAutomationId($inputBox,'NrInput_'+$name)
                $slider.Add_ValueChanged({param($sender,$eventArgs); Change-Knob ([string]$sender.Tag) $sender.Value})
                # A click gives the slider the keyboard, so the arrows move the one just touched.
                $slider.Add_PreviewMouseLeftButtonDown({param($sender,$eventArgs); $null=$sender.Focus()})
                $inputBox.Add_LostKeyboardFocus({param($sender,$eventArgs); Commit-KnobInput $sender})
                $inputBox.Add_KeyDown({param($sender,$eventArgs); if($eventArgs.Key -eq [Windows.Input.Key]::Enter) { Commit-KnobInput $sender; $eventArgs.Handled=$true }})
            }
            $target=$(if($name -in $script:MainKnobs){$script:Ui.KnobRows}else{$script:Ui.MoreKnobRows})
            $null=$target.Children.Add($grid); $script:KnobUi[$name]=$row
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
    if(-not $script:Strings.ContainsKey($Language)) { throw 'Unsupported setup language' }
    $script:ChangingLanguage=$true
    try {
        $script:Language=$Language
        # The index, or English when the tag is not in the list.
        #
        # `IndexOf` returns -1 for a language the combo does not offer, and `SelectedIndex = -1`
        # leaves a ComboBox showing NOTHING - the control is blank, which is exactly the reported
        # "the language box shows nothing". It happened because the combo's items were hardcoded in
        # XAML with three entries while the language table had four, so a Chinese system found no
        # `zh` item to select. The list is now built from the table (see below), but the fallback
        # stays: an unsupported language should show English, not an empty box.
        $tags=@($script:Ui.Language.Items | ForEach-Object { [string]$_.Tag })
        $index=[array]::IndexOf($tags, $Language)
        $script:Ui.Language.SelectedIndex=$(if($index -ge 0){$index}else{0})
        # The tooltip is set by the TipBindings loop below, which already names 'Language'.
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
            Set-EffectColour $false
            Set-RuntimeText $runtimeKey $script:RuntimeValues
        }
        Show-Speed $script:LastSpeed
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
    return [ordered]@{root=$script:Root; python=$script:Ui.PythonPath.Text.Trim(); game_exe=$script:Ui.GamePath.Text.Trim(); dll=$(if($dll){$dll}else{$null}); api=$(if($script:Ui.Api.SelectedIndex -eq 1){'dxvk'}else{'vulkan'}); game_args_raw=$script:Ui.GameArgs.Text; disable_fossilize=[bool]$script:Ui.Fossilize.IsChecked
        nr_async=[bool]$script:Ui.AsyncMode.IsChecked; nr_every=[int]$script:Ui.EveryFrame.SelectedItem.Tag;
        gpu_image_path=[bool]$script:Ui.GpuImagePath.IsChecked}
}
function Apply-Profile($Profile) {
    if($null -eq $Profile) { return }
    $script:LastProfile=$Profile
    $script:Ui.PythonPath.Text=[string]$Profile.python
    $script:Ui.GamePath.Text=[string]$Profile.game_exe
    $script:Ui.DllPath.Text=[string]$Profile.dll
    $script:Ui.Api.SelectedIndex=$(if($Profile.api -eq 'dxvk'){1}else{0})
    $script:Ui.Fossilize.IsChecked=[bool]$Profile.disable_fossilize
    if($Profile.game_args) { $script:Ui.GameArgs.Text=(@($Profile.game_args | ForEach-Object { Quote-Argument ([string]$_) }) -join ' ') }
    # A profile from before frame pacing has neither: every frame, in step with the game.
    $script:ApplyingProfile=$true
    try {
        $script:Ui.AsyncMode.IsChecked=[bool]$Profile.nr_async
        $script:Ui.GpuImagePath.IsChecked=[bool]$Profile.gpu_image_path
        $every=[int]$Profile.nr_every
        $script:Ui.EveryFrame.SelectedIndex=$(if($every -ge 1 -and $every -le $script:Ui.EveryFrame.Items.Count){$every-1}else{0})
    } finally { $script:ApplyingProfile=$false }
}
# Asynchronous mode and every Nth frame are read by the layer when the game starts: a change
# is saved at once, into the installed game's nr-env.txt too, and waits for a running action.
function Request-Pacing {
    if($script:ApplyingProfile) { return }
    if($script:Busy) { $script:PacingDue=$true } else { $script:PacingDue=$false; Start-Bridge 'pacing' }
}
function Set-Busy([bool]$Busy) {
    foreach($name in @('DllPath','GamePath','PythonPath','Api','GameArgs','Fossilize','BrowseDll','BrowseGame','BrowsePython','FindPython','Check','Dependencies','Install','Uninstall','Launch','Toggle','SpeedTest','Report','ControlsToggle','ControlsLaunch','ResetSettings','AsyncMode','GpuImagePath','EveryFrame')) { $script:Ui[$name].IsEnabled=-not $Busy }
    $script:Ui.KnobRows.IsEnabled=(-not $Busy -or ($script:Busy -and $script:Busy.Action -eq 'settings-save'))
    # An indeterminate ProgressBar animates while collapsed too: 6 % of a core, the window idle.
    $script:Ui.Spinner.IsIndeterminate=$Busy
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
function Set-EffectColour([bool]$Enabled) {
    $key=$(if($Enabled){'Good'}else{'Muted'})
    foreach($name in 'EffectState','ControlsEffectState') { $script:Ui[$name].SetResourceReference([Windows.Controls.TextBlock]::ForegroundProperty,$key) }
}
function Format-Warnings($Value) {
    # Each in this language when it has a sentence here, else as the backend words it.
    foreach($item in @($Value.warnings)) {
        if(-not $item) { continue }
        $key='Warn.'+[string]$item.name
        if($script:Strings[$script:Language].ContainsKey($key)) { [string]::Format((T $key),[object[]]@($item.values)) }
        else { [string]$item.detail }
    }
}
function Show-Speed($Value) {
    # The last speed test: the network's own time for a frame at each size and render scale, the
    # frame rate that time allows, and why a game's stays below it.
    $script:LastSpeed=$Value
    $cases=@($Value.cases | Where-Object { $_ -and ($_.network_ms -or $_.round_trip_ms) })
    if(-not $cases.Count) { $script:Ui.SpeedState.Text=''; return }
    $lines=@([string]::Format((T 'SpeedHead'),[string]$Value.device))
    foreach($scale in @($cases | ForEach-Object { [double]$_.render_scale } | Select-Object -Unique)) {
        $items=@($cases | Where-Object { [double]$_.render_scale -eq $scale } | ForEach-Object {
            $ms=$(if($_.network_ms){[double]$_.network_ms}else{[double]$_.round_trip_ms})
            [string]::Format((T 'SpeedCase'),$_.width,$_.height,[Math]::Round($ms),[Math]::Floor(1000/$ms)) })
        $lines+=[string]::Format((T 'SpeedScale'),$scale.ToString('0.0#',[Globalization.CultureInfo]::InvariantCulture),($items -join '; '))
    }
    $lines+=(T 'SpeedNote')
    $script:Ui.SpeedState.Text=($lines -join "`n")
}
function Show-Status($Value) {
    $enabled=[bool]$Value.trigger_exists
    if($null -ne $Value.effect_on) { $enabled=[bool]$Value.effect_on }
    $script:LastStatus=$Value
    $script:Ui.EffectState.Text=$(if($enabled){T 'EffectOn'}else{T 'EffectOff'})
    $script:Ui.Toggle.Content=$(if($enabled){T 'Disable'}else{T 'Enable'})
    $script:Ui.ControlsToggle.Content=$script:Ui.Toggle.Content
    $script:Ui.ControlsEffectState.Text=$script:Ui.EffectState.Text
    Set-EffectColour $enabled
    $count=$Value.processed
    if($null -eq $count -and $Value.counts) { $count=$Value.counts.processed }
    $refused=$Value.rejected
    if($null -eq $refused -and $Value.counts) { $refused=$Value.counts.rejected }
    if($null -eq $count) { $count=0 }
    if($null -eq $refused) { $refused=0 }
    $shape=''
    if($Value.network_shape -and $Value.network_shape.Count -eq 2) { $shape=[string]::Format((T 'Network'),$Value.network_shape[0],$Value.network_shape[1]) }
    $kind=$(if($Value.counts_complete -eq $false){T 'TailFrames'}else{T 'Frames'})
    if($Value.game_elevated -and -not $Value.fresh_frames) {
        # From the proxy's record: as administrator the game never loads NR's layer.
        Set-RuntimeText $(if($Value.uac_enabled -eq $false){'GameElevatedUacOff'}else{'GameElevated'})
    } elseif($Value.fresh_frames -or $Value.game_connected) {
        Set-RuntimeText 'Processing' @($kind,$count,$refused,$shape)
    } elseif([int]$count -gt 0) {
        Set-RuntimeText 'Historical' @($count,$refused)
    } else { Set-RuntimeText 'WaitingFrames' }
    if($Value.PSObject.Properties['speed_test']) { Show-Speed $Value.speed_test }
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
            if($Job.Action -in @('install','uninstall') -and $value.steam_restored) { Set-Progress ('Done.'+$Job.Action+'-steam') }
            if($Job.Action -eq 'on' -and $value.warning -eq 'game_without_nr') { Set-Progress 'GameWithoutNr' }
            if($Job.Action -eq 'on' -and $value.warning -eq 'game_elevated') { Set-Progress $(if($value.uac_enabled -eq $false){'GameElevatedUacOff'}else{'GameElevated'}) }
            if($Job.Action -eq 'pacing' -and -not $value.applied) { Set-Progress 'Done.pacing-later' }
            if($Job.Action -eq 'speedtest') {
                Show-Speed $value.speed_test
                $script:Ui.Progress.Text=(T 'Done.speedtest')+"`n"+$script:Ui.SpeedState.Text
            }
            if($Job.Action -in @('check','install')) {
                # What would keep NR from the game without stopping the install, said here rather
                # than left in the details.
                $notes=@(Format-Warnings $value)
                if($notes.Count) { $script:Ui.Progress.Text=$script:Ui.Progress.Text+' '+($notes -join ' ') }
            }
            if($value.profile -and $Job.Action -in @('discover','dependencies','install')) { Apply-Profile $value.profile }
            if($Job.Action -eq 'discover' -and -not $value.profile -and $value.candidates.Count) { $script:Ui.PythonPath.Text=[string]$value.candidates[0] }
            if($Job.Action -eq 'install') { $script:Installed=$true }
            if($Job.Action -eq 'uninstall') { $script:Installed=$false }
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
            $failed=@($value.checks | Where-Object { -not $_.ok } | ForEach-Object { [string]$_.name })
            if($Job.Action -eq 'check' -and $failed.Count -eq 1 -and $failed[0] -eq 'python_packages') {
                # Install prepares a Python of its own when only the packages are missing.
                Set-Progress 'PackagesLater'
            } elseif($failed.Count -or -not $value.error) {
                Set-Progress 'FixChecks'
                # The items themselves after ours, as the backend words them: a user who saw
                # only "fix the reported items" had a JSON to read to learn which (issue #12).
                $items=@($value.checks | Where-Object { -not $_.ok } | ForEach-Object { [string]$_.name+': '+[string]$_.detail })
                if($items.Count) { $script:Ui.Progress.Text=(T 'FixChecks')+' '+($items -join ' | ') }
                $script:Ui.CheckDetails.IsExpanded=$true
            } else {
                # The backend's own sentence after ours: it names the game, Steam or file at fault.
                Set-Progress 'ActionFailed'
                $script:Ui.Progress.Text=(T 'ActionFailed')+' '+[string]$value.error
            }
            Restore-SettingsFailure $Job
        }
    }
    $script:Busy=$null; Set-Busy $false
    if($Job.Action -eq 'discover') { $script:Ui.MainScroll.ScrollToTop() }
    # A first installation is followed by the speed test, once: what NR does here at its best. The
    # timer starts it after the settings the installation reloads, so its words are the last.
    if($Job.Action -eq 'install' -and $value -and $value.ok -and $value.speed_test_due) { $script:SpeedTestDue=$true }
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


try { $prefs=Read-Json $script:UiPrefs } catch { $prefs=$null }
Set-Theme $(if($prefs -and $prefs.theme -in @('light','dark')){[string]$prefs.theme}else{Get-SystemTheme})
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
        # The keyboard on a slider: an arrow is one step, Page Up five. The window is shown
        # off screen for it, since keys need a presentation source; the change is not kept.
        $script:Window.WindowStartupLocation='Manual'; $script:Window.Left=-20000; $script:Window.Top=-20000; $script:Window.ShowActivated=$false
        $script:Window.Show(); $script:Ui.Pages.SelectedItem=$script:Ui.ControlsTab; $script:Window.UpdateLayout()
        $row=$script:KnobUi['intensity']; $start=$row.Slider.Value
        $source=[Windows.PresentationSource]::FromVisual($row.Slider)
        foreach($key in 'Right','Right','PageUp','Left') {
            $press=[Windows.Input.KeyEventArgs]::new([Windows.Input.Keyboard]::PrimaryDevice,$source,0,[Windows.Input.Key]$key)
            $press.RoutedEvent=[Windows.Input.Keyboard]::KeyDownEvent
            $row.Slider.RaiseEvent($press)
        }
        $expected=$start+6*[double]$row.Model.step
        if([Math]::Abs($row.Slider.Value-$expected) -gt 1e-9) { throw "Slider keys gave $($row.Slider.Value), expected $expected" }
        Set-KnobDisplay 'intensity' $start; $script:PendingSettings=@{}
        $script:Window.Hide()
    }
    foreach($theme in 'dark','light') {
        Set-Theme $theme
        if($script:Window.Resources['Text'].Color.ToString() -ne $script:Themes[$theme].Text.Replace('#','#FF')) { throw "Theme $theme was not applied" }
    }
    # Not $language: names ignore case, and at script scope that is $script:Language itself.
    $autoLanguage=$script:Language
    $languages=@($script:Ui.Language.Items | ForEach-Object { [string]$_.Tag })
    if((($languages | Sort-Object) -join ',') -ne ((@($script:Strings.Keys) | Sort-Object) -join ',')) { throw 'The language list and the translations differ' }
    foreach($code in $languages) {
        foreach($key in $script:Strings.en.Keys) {
            if(-not $script:Strings[$code].ContainsKey($key) -or -not $script:Strings.en[$key] -or -not $script:Strings[$code][$key]) { throw "Incomplete translation ($code): $key" }
            if($code -ne 'ru' -and $script:Strings[$code][$key] -match '[А-Яа-яЁё]') { throw "Russian text in the $code translation: $key" }
        }
        if($script:Strings.en.Count -ne $script:Strings[$code].Count) { throw "Translation key counts differ ($code)" }
    }
    foreach($language in $languages) {
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
        Show-Status ([pscustomobject]@{trigger_exists=$true;processed=0;rejected=0;fresh_frames=$false;game_elevated=$true;uac_enabled=$true})
        if($script:Ui.RuntimeState.Text -ne (T 'GameElevated')) { throw 'An elevated game was not said' }
        Show-Status ([pscustomobject]@{trigger_exists=$true;processed=0;rejected=0;fresh_frames=$false;game_elevated=$true;uac_enabled=$false})
        if($script:Ui.RuntimeState.Text -ne (T 'GameElevatedUacOff')) { throw 'An elevated game with UAC off was not said' }
        foreach($key in @($script:Strings[$language].Keys | Where-Object { $_ -like 'Warn.*' })) { $null=[string]::Format((T $key),'A.exe','B.exe') }
        $notes=@(Format-Warnings ([pscustomobject]@{warnings=@(
            [pscustomobject]@{name='vulkan_executable_other';detail='';values=@('DOOMx64.exe','DOOMx64vk.exe')},
            [pscustomobject]@{name='not_worded_here';detail='As the backend words it';values=@()})}))
        if($notes.Count -ne 2 -or $notes[0] -notmatch 'DOOMx64\.exe' -or $notes[0] -notmatch 'DOOMx64vk\.exe' -or $notes[1] -ne 'As the backend words it') { throw 'Warnings were not worded' }
        Show-Speed ([pscustomobject]@{device='Fixture GPU';cases=@(
            [pscustomobject]@{width=640;height=360;render_scale=0.4;network_ms=23;round_trip_ms=26.9},
            [pscustomobject]@{width=1920;height=1080;render_scale=1.0;network_ms=384;round_trip_ms=408.6})})
        $speed=$script:Ui.SpeedState.Text
        $missing=@('640×360','\b23\b','\b43\b','\b384\b','0\.4','1\.0','Fixture GPU' | Where-Object { $speed -notmatch $_ })
        if($missing.Count -or -not $speed.EndsWith((T 'SpeedNote'))) { throw "The speed test was not worded ($($missing -join ', ')): $speed" }
        Show-Speed $null
        if($script:Ui.SpeedState.Text) { throw 'No speed test left words behind' }
        # Frame pacing reaches the backend, a profile without it is every frame in step, and
        # neither a profile applied to the window nor a change during an action starts a save.
        Apply-Profile ([pscustomobject]@{python='fixture';game_exe='fixture.exe';api='vulkan';disable_fossilize=$true;nr_async=$true;nr_every=3;gpu_image_path=$true})
        $sent=Profile-FromWindow
        if(-not $sent.nr_async -or $sent.nr_every -ne 3 -or -not $sent.gpu_image_path -or $script:PacingDue) { throw 'Frame pacing and GPU image choice did not reach the profile' }
        Apply-Profile ([pscustomobject]@{python='fixture';game_exe='fixture.exe';api='vulkan';disable_fossilize=$true})
        $sent=Profile-FromWindow
        if($sent.nr_async -or $sent.nr_every -ne 1 -or $sent.gpu_image_path) { throw 'An older profile was not every frame, in step, with GPU image path off' }
        Set-Progress 'ChooseFiles'
        $script:ApplyingProfile=$true; Request-Pacing; $script:ApplyingProfile=$false
        if($script:PacingDue -or $script:ProgressKey -ne 'ChooseFiles') { throw 'Applying a profile asked to save frame pacing' }
        $script:Busy=[pscustomobject]@{Action='check'}; Request-Pacing; $script:Busy=$null
        if(-not $script:PacingDue) { throw 'Frame pacing changed during an action was not kept for later' }
        # Free, it goes to the bridge at once: here with no Python behind the fixture's path.
        Request-Pacing
        if($script:PacingDue -or $script:ProgressKey -ne 'PythonMissing') { throw 'Frame pacing was not saved when free' }
        foreach($item in $script:Ui.EveryFrame.Items) { if(-not $item.Content) { throw 'Untranslated frame pacing choice' } }
        Set-Language $(if($language -eq 'en'){'ru'}else{'en'})
        if($script:Ui.Toggle.Content -ne (T 'Disable')) { throw 'Language switch changed the displayed effect state' }
        Show-Status ([pscustomobject]@{trigger_exists=$false;processed=12;rejected=1;fresh_frames=$false})
        if($script:Ui.Toggle.Content -ne (T 'Enable')) { throw 'Localized disabled status failed' }
        Set-Progress 'Loading.install'
        Set-RuntimeText 'StatusUnavailable'
        Set-Language $language
        if($script:Ui.Progress.Text -ne (T 'Loading.install') -or $script:Ui.RuntimeState.Text -ne (T 'StatusUnavailable')) { throw 'Dynamic messages did not switch languages' }
    }
    Write-Output ('WPF layout loaded; '+$script:Ui.Count+' named controls; '+$script:KnobUi.Count+' live NR controls. '+($languages -join '/').ToUpper()+' coverage: '+$script:Strings.en.Count+' keys; automatic language: '+$autoLanguage+'.')
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
$script:Ui.Uninstall.Add_Click({
    if([Windows.MessageBox]::Show($script:Window,(T 'UninstallConfirm'),(T 'Uninstall'),'OKCancel','Question') -eq 'OK') { Start-Bridge 'uninstall' }
})
$script:Ui.Launch.Add_Click({ Start-Bridge 'launch' })
$script:Ui.SpeedTest.Add_Click({ Start-Bridge 'speedtest' })
$script:Ui.Toggle.Add_Click({ $action=$(if($script:Ui.Toggle.Content -eq (T 'Disable')){'off'}else{'on'}); Start-Bridge $action })
$script:Ui.ControlsToggle.Add_Click({ $action=$(if($script:Ui.ControlsToggle.Content -eq (T 'Disable')){'off'}else{'on'}); Start-Bridge $action })
$script:Ui.ControlsLaunch.Add_Click({ Start-Bridge 'launch' })
$script:Ui.ResetSettings.Add_Click({ Reset-NrSettings })
# Checked and Unchecked rather than Click: UI Automation's Toggle, which screen readers use,
# changes the box without a click.
$script:Ui.AsyncMode.Add_Checked({ Request-Pacing })
$script:Ui.AsyncMode.Add_Unchecked({ Request-Pacing })
$script:Ui.GpuImagePath.Add_Checked({ Request-Pacing })
$script:Ui.GpuImagePath.Add_Unchecked({ Request-Pacing })
$script:Ui.EveryFrame.Add_SelectionChanged({ Request-Pacing })
$script:Ui.Theme.Add_Click({
    Set-Theme $(if($script:Theme -eq 'dark'){'light'}else{'dark'})
    try { [IO.Directory]::CreateDirectory($script:Work) | Out-Null; Write-Json $script:UiPrefs @{theme=$script:Theme} } catch { }
})
$script:Window.Add_SourceInitialized({ Set-TitleBar })
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
        if(-not $script:Busy -and -not $script:SettingsReload -and $script:SpeedTestDue) { $script:SpeedTestDue=$false; Start-Bridge 'speedtest' }
        if(-not $script:Busy -and $script:PacingDue) { Request-Pacing }
        if(-not $script:Busy -and $script:PendingSettings.Count -and [DateTime]::UtcNow -ge $script:SettingsDue) {
            $dragging=$false
            foreach($row in $script:KnobUi.Values) { if($row.Slider -and $row.Slider.IsMouseCaptureWithin) { $dragging=$true; break } }
            if(-not $dragging) { Save-NrSettings }
        }
        $script:TickCount++
        # Status every 5 s while the window is in front, and at once when it comes back. Each
        # check starts Python, and behind a running game nobody reads the answer. A window that
        # never had the foreground counts itself active, so minimised is checked as well.
        $due=$script:StatusDue -or $script:TickCount % 10 -eq 0
        $front=$script:Window.IsActive -and $script:Window.WindowState -ne 'Minimized'
        if($due -and $front -and -not $script:Busy -and -not $script:StatusJob -and $script:Ui.GamePath.Text -and $script:Ui.PythonPath.Text) { $script:StatusDue=$false; Start-Bridge 'status' '' $true }
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
$script:Window.Add_Activated({ $script:StatusDue=$true })
$script:Window.Add_StateChanged({ if($script:Window.WindowState -ne 'Minimized') { $script:StatusDue=$true } })
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
