using System;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;

internal static class EnhancementSettings
{
    private static readonly string DataPath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "Avadon3Chinese");
    private static readonly string FontPath = Path.Combine(DataPath, "enhancement.ini");

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern uint GetPrivateProfileInt(string section, string key, int fallback, string path);

    internal sealed class Options
    {
        internal int Language, Scale, Movement, Markers;
        internal bool FitLabels, Inventory, Camera;
    }

    internal static Options ReadOptions(string path)
    {
        bool firstUse = !File.Exists(path);
        return new Options {
            Language = (int)Math.Min(2u, GetPrivateProfileInt("Settings", "Language", 0, path)),
            Scale = (int)Math.Min(200u, Math.Max(100u, GetPrivateProfileInt("Font", "ScalePercent", firstUse ? 150 : 100, path))),
            FitLabels = GetPrivateProfileInt("Font", "FitLabels", 1, path) != 0,
            Inventory = GetPrivateProfileInt("Inventory", "ToggleHotkey", firstUse ? 1 : 0, path) != 0,
            Camera = GetPrivateProfileInt("Camera", "FollowCharacter", firstUse ? 1 : 0, path) != 0,
            Movement = (int)Math.Min(200u, Math.Max(100u, GetPrivateProfileInt("Movement", "SpeedPercent", firstUse ? 200 : 100, path))),
            Markers = (int)Math.Min(2u, GetPrivateProfileInt("Interaction", "HighlightOnShift", firstUse ? 2 : 0, path))
        };
    }

    [STAThread]
    private static void Main()
    {
        Application.EnableVisualStyles();
        if (!File.Exists(Path.Combine(DataPath, "resources.bin")))
        {
            MessageBox.Show("Install the Avadon 3 Chinese patch first. 请先安装阿瓦登 3 汉化补丁。");
            return;
        }
        var options = ReadOptions(FontPath);

        using (var form = new Form { Width = 510, Height = 415, StartPosition = FormStartPosition.CenterScreen })
        using (var layout = new FlowLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(18),
                FlowDirection = FlowDirection.TopDown, WrapContents = false, AutoScroll = true })
        {
            var language = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 150 };
            language.Items.AddRange(new object[] { "System / 系统", "简体中文", "English" });
            language.SelectedIndex = options.Language;
            var scaleLabel = new Label { AutoSize = true };
            var scale = new NumericUpDown { Minimum = 100, Maximum = 200,
                Value = options.Scale };
            var fit = new CheckBox { AutoSize = true,
                Checked = options.FitLabels };
            var inventory = new CheckBox { AutoSize = true,
                Checked = options.Inventory };
            var camera = new CheckBox { AutoSize = true,
                Checked = options.Camera };
            var movementLabel = new Label { AutoSize = true };
            var movement = new NumericUpDown { Minimum = 100, Maximum = 200,
                Value = options.Movement };
            var markerLabel = new Label { AutoSize = true };
            var markers = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 180 };
            int markerMode = options.Markers;
            var apply = new Button { AutoSize = true };
            var status = new Label { AutoSize = true };
            Func<bool> english = () => language.SelectedIndex == 2 || (language.SelectedIndex == 0
                && CultureInfo.CurrentUICulture.TwoLetterISOLanguageName != "zh");
            Action updateLanguage = () =>
            {
                bool en = english();
                form.Text = en ? "Avadon 3 Enhancement Settings" : "阿瓦登 3 体验优化设置";
                scaleLabel.Text = en ? "Chinese font scale (%)" : "中文字号缩放（%）";
                fit.Text = en ? "Fit short labels to their original space" : "短标签自动适配原有空间";
                inventory.Text = en ? "Press the inventory shortcut again to close it" : "再按一次背包快捷键关闭背包";
                camera.Text = en ? "Follow the player with the camera" : "镜头跟随角色";
                movementLabel.Text = en ? "Party movement in exploration (%)" : "探索时队伍移动速度（%）";
                markerLabel.Text = en ? "Interactable object markers" : "可互动物体标记";
                int selectedMarker = markers.SelectedIndex < 0 ? markerMode : markers.SelectedIndex;
                markers.Items.Clear();
                markers.Items.AddRange(en ? new object[] { "Off", "While holding Shift", "Always" }
                    : new object[] { "关闭", "按住 Shift 显示", "始终显示" });
                markers.SelectedIndex = selectedMarker;
                apply.Text = en ? "Save settings" : "保存设置";
                status.Text = "";
            };
            language.SelectedIndexChanged += (sender, args) => updateLanguage();
            apply.Click += (sender, args) =>
            {
                try
                {
                    string temporary = Path.Combine(DataPath, Guid.NewGuid().ToString("N") + ".tmp");
                    try
                    {
                        File.WriteAllText(temporary, String.Format(CultureInfo.InvariantCulture,
                            "[Font]\r\nScalePercent={0}\r\nFitLabels={1}\r\n[Settings]\r\nLanguage={2}\r\n[Inventory]\r\nToggleHotkey={3}\r\n[Camera]\r\nFollowCharacter={4}\r\n[Movement]\r\nSpeedPercent={5}\r\n[Interaction]\r\nHighlightOnShift={6}\r\n",
                            scale.Value, fit.Checked ? 1 : 0, language.SelectedIndex,
                            inventory.Checked ? 1 : 0, camera.Checked ? 1 : 0, movement.Value, markers.SelectedIndex), Encoding.ASCII);
                        if (File.Exists(FontPath)) File.Replace(temporary, FontPath, null);
                        else File.Move(temporary, FontPath);
                    }
                    finally { if (File.Exists(temporary)) File.Delete(temporary); }
                    status.Text = english() ? "Saved. Applies next time the game starts." : "已保存，下次启动游戏时生效。";
                }
                catch (IOException error) { status.Text = error.Message; }
                catch (UnauthorizedAccessException error) { status.Text = error.Message; }
            };
            layout.Controls.AddRange(new Control[] { language, scaleLabel, scale, fit, inventory, camera,
                movementLabel, movement, markerLabel, markers, apply, status });
            form.Controls.Add(layout);
            updateLanguage();
            Application.Run(form);
        }
    }
}
