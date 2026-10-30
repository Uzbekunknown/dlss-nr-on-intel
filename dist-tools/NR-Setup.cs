using System;
using System.IO;
using System.Management.Automation;
using System.Management.Automation.Runspaces;
using System.Reflection;
using System.Threading;
using System.Windows.Forms;

// A console-free host for the same WPF window as NR-Setup.cmd. Python is only
// needed by the backend, so the setup window can explain a missing interpreter.
internal static class NrSetup
{
    // The two strings this host owns, in the four languages the wizard itself supports. Everything
    // else lives in windows-wizard.ps1 and is translated there; these two are here because they
    // exist before the script is loaded, or when it fails to load at all - the one path where the
    // script cannot translate itself.
    private static readonly string[] Titles = { "DLSS-NR setup", "DLSS-NR 安装程序", "Установка DLSS-NR", "Instalación de DLSS-NR" };
    private static readonly string[] ExtractErrors =
    {
        "Extract the complete DLSS-NR ZIP before starting setup.",
        "请先解压完整的 DLSS-NR 压缩包，再开始安装。",
        "Распакуйте полный ZIP-архив DLSS-NR перед началом установки.",
        "Extraiga el ZIP completo de DLSS-NR antes de iniciar la instalación."
    };

    // Which of the four to use, from the Windows UI language. A system language that is none of
    // them falls back to English, which is what the wizard does too.
    private static int LanguageIndex()
    {
        string two;
        try { two = System.Globalization.CultureInfo.CurrentUICulture.TwoLetterISOLanguageName; }
        catch { return 0; }
        if (two == "zh") return 1;
        if (two == "ru") return 2;
        if (two == "es") return 3;
        return 0;
    }

    [STAThread]
    private static int Main()
    {
        try
        {
            string root = Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location);
            string source = Path.Combine(root, "windows-wizard.ps1");
            if (!File.Exists(source))
                throw new FileNotFoundException(ExtractErrors[LanguageIndex()], source);
            using (Runspace runspace = RunspaceFactory.CreateRunspace())
            {
                runspace.ApartmentState = ApartmentState.STA;
                runspace.ThreadOptions = PSThreadOptions.UseCurrentThread;
                runspace.Open();
                using (PowerShell command = PowerShell.Create())
                {
                    command.Runspace = runspace;
                    command.AddScript(File.ReadAllText(source), true).AddParameter("Root", root);
                    command.Invoke();
                    if (command.HadErrors)
                        throw new InvalidOperationException(command.Streams.Error[0].ToString());
                }
            }
            return 0;
        }
        catch (Exception error)
        {
            MessageBox.Show(error.Message, Titles[LanguageIndex()], MessageBoxButtons.OK, MessageBoxIcon.Error);
            return 1;
        }
    }
}
