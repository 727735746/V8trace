using System;
using System.Collections;
using System.Collections.Generic;
using System.Windows.Forms;

namespace V8Trace {
    public sealed partial class TraceWindow {
        readonly HashSet<string> editionRestricted = new HashSet<string>(StringComparer.Ordinal);
        bool advancedAnalysis = true;
        string editionLabel = "Professional 专业版";

        void ApplyEdition(Dictionary<string, object> message) {
            var edition = ObjectValue(message, "edition");
            if (Value(edition, "protocol") != "v8trace/edition/1") throw new InvalidOperationException("采集内核未声明编译版本身份，请使用配套内核。");
            var id = Value(edition, "id");
            if (id != "community" && id != "professional") throw new InvalidOperationException("未知采集内核版本身份。");
            editionRestricted.Clear();
            object keys;
            if (edition.TryGetValue("restrictedCaptureOptions", out keys)) foreach (var key in (IEnumerable)keys) editionRestricted.Add(Convert.ToString(key));
            advancedAnalysis = Value(edition, "advancedAnalysis") == "True";
            editionLabel = Value(edition, "label");
            Text = "V8 Trace Studio · " + editionLabel + " · Alcohol";
            foreach (var item in channels) {
                if (editionRestricted.Contains(item.Key) && !item.Value.Text.EndsWith("[专业版]", StringComparison.Ordinal)) item.Value.Text += " [专业版]";
            }
            if (!advancedAnalysis) causalStatus.Text = "Community 保留完整原始日志检索；HTTP 根链联合分析属于 Professional。";
        }
        bool PresetAvailable(ScenarioChoice choice) { return choice == null || Value(choice.Record, "available") != "False"; }
        void ApplyEditionAvailability() {
            foreach (var item in channels) if (editionRestricted.Contains(item.Key)) item.Value.Enabled = false;
            if (!advancedAnalysis) {
                foreach (var button in studioActions) if (button.Name.StartsWith("Causal", StringComparison.Ordinal)) button.Enabled = false;
                foreach (Control input in new Control[] { vmpProbeFile, vmpDepth, vmpNodes, vmpProperties, vmpTypedMiB, vmpMaxActive }) input.Enabled = false;
            }
        }
    }
}
