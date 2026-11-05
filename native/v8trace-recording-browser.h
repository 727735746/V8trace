#ifndef CHROME_BROWSER_V8TRACE_RECORDING_BROWSER_H_
#define CHROME_BROWSER_V8TRACE_RECORDING_BROWSER_H_
#include <memory>
#include <vector>
#include "chrome/browser/v8trace-recording-control.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"

namespace v8trace_recording {
inline content::WebContents* Contents() {
  content::WebContents* found = nullptr;
  auto* collection = GlobalBrowserCollection::GetInstance();
  if (!collection) return nullptr;
  collection->ForEach([&](BrowserWindowInterface* browser) {
    auto* tabs = browser->GetTabStripModel();
    if (tabs && tabs->GetActiveWebContents()) { found = tabs->GetActiveWebContents(); return false; }
    return true;
  });
  return found;
}
inline network::mojom::NetworkContext* GetNetworkContext() {
  auto* contents = Contents();
  return contents ? contents->GetBrowserContext()->GetDefaultStoragePartition()->GetNetworkContext() : nullptr;
}
inline bool Navigate(const GURL& url) {
  auto* contents = Contents();
  if (!contents) return false;
  content::NavigationController::LoadURLParams params(url);
  contents->GetController().LoadURLWithParams(params); return true;
}
class HttpDrainNavigation final : public content::WebContentsObserver {
 public:
  HttpDrainNavigation(content::WebContents* contents, base::OnceCallback<void(bool)> done)
      : content::WebContentsObserver(contents), done_(std::move(done)) {}
  void Start() {
    deadline_.Start(FROM_HERE, base::Seconds(30), base::BindOnce(&HttpDrainNavigation::Finish, base::Unretained(this), false));
    content::NavigationController::LoadURLParams params(GURL("about:blank"));
    web_contents()->GetController().LoadURLWithParams(params);
  }
  void DidFinishNavigation(content::NavigationHandle* navigation) override {
    if (navigation->IsInPrimaryMainFrame() && navigation->HasCommitted())
      Finish(navigation->GetURL() == GURL("about:blank") && !navigation->IsErrorPage());
  }
  void WebContentsDestroyed() override { Finish(false); }
 private:
  void Finish(bool success) {
    deadline_.Stop(); Observe(nullptr); auto done = std::move(done_); delete this;
    std::move(done).Run(success);
  }
  base::OnceCallback<void(bool)> done_;
  base::OneShotTimer deadline_;
};
inline bool DrainHttp(base::OnceCallback<void(bool)> done) {
  auto* collection = GlobalBrowserCollection::GetInstance();
  if (!collection) return false;
  std::vector<base::WeakPtr<content::WebContents>> pages;
  collection->ForEach([&](BrowserWindowInterface* browser) {
    if (auto* tabs = browser->GetTabStripModel())
      for (int index = 0; index < tabs->count(); ++index) if (auto* contents = tabs->GetWebContentsAt(index)) pages.push_back(contents->GetWeakPtr());
    return true;
  });
  if (pages.empty()) return false;
  auto remaining = std::make_shared<size_t>(pages.size());
  auto success = std::make_shared<bool>(true);
  auto completed = std::make_shared<base::OnceCallback<void(bool)>>(std::move(done));
  for (auto contents : pages) {
    auto callback = base::BindOnce([](std::shared_ptr<size_t> count, std::shared_ptr<bool> result,
        std::shared_ptr<base::OnceCallback<void(bool)>> complete, bool committed) {
      *result = *result && committed;
      if (!--*count) std::move(*complete).Run(*result);
    }, remaining, success, completed);
    if (contents) (new HttpDrainNavigation(contents.get(), std::move(callback)))->Start();
    else std::move(callback).Run(false);
  }
  return true;
}
}
#endif
