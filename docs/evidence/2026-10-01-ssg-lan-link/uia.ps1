param(
    [Parameter(Mandatory = $true)][string]$Action,   # windows | dump | click | select | text
    [string]$Name = "",
    [string]$Window = "",
    [string]$Filter = ""
)
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
$AE = [System.Windows.Automation.AutomationElement]
$TS = [System.Windows.Automation.TreeScope]
$proc = Get-Process mib_studio_qt -ErrorAction Stop | Select-Object -First 1
$cond = New-Object System.Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $proc.Id)
$wins = @($AE::RootElement.FindAll($TS::Children, $cond))
# Dialogs owned by the main window are children of it in the UIA tree.
$all = @()
foreach ($w in $wins) {
    $all += $w
    $all += @($w.FindAll($TS::Children, (New-Object System.Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [System.Windows.Automation.ControlType]::Window))))
}
function Describe($e) {
    $e.Current.ControlType.ProgrammaticName.Replace("ControlType.", "") + " | " + $e.Current.Name + " | " + $e.Current.AutomationId + " | enabled=" + $e.Current.IsEnabled
}
function Target {
    if ($Window) { return $all | Where-Object { $_.Current.Name -eq $Window } | Select-Object -First 1 }
    return $wins[0]
}
switch ($Action) {
    "windows" { $all | ForEach-Object { Describe $_ } }
    "dump" {
        $t = Target
        @($t.FindAll($TS::Descendants, [System.Windows.Automation.Condition]::TrueCondition)) |
            ForEach-Object { Describe $_ } | Where-Object { -not $Filter -or $_ -match $Filter }
    }
    "text" {
        $t = Target
        @($t.FindAll($TS::Descendants, (New-Object System.Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [System.Windows.Automation.ControlType]::Text)))) |
            ForEach-Object { $_.Current.Name } | Where-Object { $_ }
    }
    "click" {
        $t = Target
        $e = @($t.FindAll($TS::Descendants, (New-Object System.Windows.Automation.PropertyCondition($AE::NameProperty, $Name)))) |
            Where-Object { $_.Current.ControlType -eq [System.Windows.Automation.ControlType]::Button -or $_.Current.ControlType -eq [System.Windows.Automation.ControlType]::MenuItem } |
            Select-Object -First 1
        if (-not $e) { "NOT FOUND: $Name"; exit 1 }
        Describe $e
        $e.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
        "invoked"
    }
    "select" {
        $t = Target
        $e = @($t.FindAll($TS::Descendants, (New-Object System.Windows.Automation.PropertyCondition($AE::NameProperty, $Name)))) |
            Where-Object { $_.Current.ControlType -eq [System.Windows.Automation.ControlType]::TabItem } | Select-Object -First 1
        if (-not $e) { "NOT FOUND: $Name"; exit 1 }
        Describe $e
        $e.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()
        "selected"
    }
}
