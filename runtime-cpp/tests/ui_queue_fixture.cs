using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Web.Script.Serialization;

public static class QueueFixture {
    static readonly JavaScriptSerializer Json = new JavaScriptSerializer { MaxJsonLength = 8 * 1024 * 1024 };
    static StreamWriter evidence;
    static int order;
    static void Send(Dictionary<string, object> value) { value["fixtureOrder"] = ++order; SendRaw(Json.Serialize(value)); }
    static void SendRaw(string value) { evidence.WriteLine(value); evidence.Flush(); Console.WriteLine(value); Console.Out.Flush(); }
    static Dictionary<string, object> Message(string type, string id) { return new Dictionary<string, object> { { "type", type }, { "id", id } }; }
    static void Control(string id, int generation) {
        var row = Message("recording-state", id); row["generation"] = generation; row["paused"] = generation % 2 == 0;
        row["pending"] = false; row["ready"] = true; row["state"] = generation % 2 == 0 ? "paused" : "recording"; Send(row);
    }
    public static void Main() {
        Console.InputEncoding = new UTF8Encoding(false); Console.OutputEncoding = new UTF8Encoding(false);
        var root = AppDomain.CurrentDomain.BaseDirectory;
        using (evidence = new StreamWriter(Path.Combine(root, "fixture-wire.jsonl"), false, new UTF8Encoding(false))) {
            SendRaw(File.ReadAllText(Path.Combine(root, "fixture-ready.json"), Encoding.UTF8).Trim());
            string line;
            while ((line = Console.ReadLine()) != null) {
                var request = Json.Deserialize<Dictionary<string, object>>(line);
                var command = Convert.ToString(request["command"]); var id = Convert.ToString(request["id"]);
                if (command == "shutdown") {
                    for (int n = 0; n < 100; ++n) Send(Message("fixture-exit", id));
                    var final = Message("worker-stderr", id); final["error"] = "fixture-final-exit-marker"; Send(final); return;
                }
                if (command == "stop") { Send(Message("stopping", id)); Send(Message("fixture-stop-ack", id)); continue; }
                if (command == "fixture-invalid") { SendRaw("{fixture-invalid"); continue; }
                if (command != "fixture-load" && command != "fixture-increment") continue;
                int first = command == "fixture-load" ? 1 : 1201, batches = command == "fixture-load" ? 120 : 2;
                for (int batch = 0; batch < batches; ++batch) {
                    var events = new List<object>();
                    for (int item = 0; item < 10; ++item) {
                        int seq = first + batch * 10 + item;
                        events.Add(new { seq = seq, category = seq % 2 == 0 ? "jscall" : "native", kind = "fixture",
                            observedAt = DateTime.UtcNow.ToString("o"), detail = "fixture " + seq + (seq % 100 == 0 ? " needle" : ""), sessionId = "fixture-session", contextId = 1 });
                    }
                    var row = Message("events", id); row["events"] = events; Send(row);
                    if (command == "fixture-load" && batch % 30 == 29) { Control(id, batch / 30); if (batch == 89) Control(id, 1); }
                    if (command == "fixture-load" && batch == 60) { row = Message("fixture-large", id); row["fullValue"] = new string('X', 3 * 1024 * 1024) + "中"; Send(row); }
                }
                Send(Message("fixture-end", id));
            }
        }
    }
}
