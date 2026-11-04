#include "lgp/platform/web/web_text_input.hpp"

#include <emscripten/em_asm.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace lgp::platform::web {

namespace {
std::mutex one_at_a_time;  // one dialog on the page at a time
}  // namespace

void WebTextInput::show_message(const MessageRequest& message) {
    std::lock_guard lock(one_at_a_time);
    std::atomic<int32_t> status{0};  // 0 open, 1 closed
    MAIN_THREAD_ASYNC_EM_ASM(
        {
            const code = UTF8ToString($0);
            const text = UTF8ToString($1);
            const statusAddr = $2;
            const d = document.createElement('dialog');
            d.className = 'lgp-message';
            d.style.cssText = 'font: 16px system-ui, sans-serif; padding: 16px; border-radius: 8px; max-width: 32em;';
            const form = document.createElement('form');
            form.method = 'dialog';
            const title = document.createElement('p');
            title.textContent = code;
            title.style.cssText = 'margin: 0 0 8px; font-weight: 600;';
            form.appendChild(title);
            if (text) {
                const p = document.createElement('p');
                p.textContent = text;
                p.style.cssText = 'margin: 0 0 8px; white-space: pre-wrap;';
                form.appendChild(p);
            }
            const buttons = document.createElement('p');
            buttons.style.cssText = 'margin: 12px 0 0; text-align: right;';
            const ok = document.createElement('button');
            ok.textContent = 'OK';
            buttons.appendChild(ok);
            form.appendChild(buttons);
            d.appendChild(form);
            d.addEventListener('close', () => {
                if (typeof growMemViews === 'function') growMemViews();
                const heap = HEAP32;
                Atomics.store(heap, statusAddr >> 2, 1);
                Atomics.notify(heap, statusAddr >> 2);
                d.remove();
            });
            document.body.appendChild(d);
            d.showModal();
            ok.focus();
        },
        message.code.c_str(), message.text.c_str(), &status);
    while (status.load() == 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

bool WebTextInput::ask(const TextRequest& request, std::string* text) {
    std::lock_guard lock(one_at_a_time);
    std::vector<char> buffer(4096);
    int32_t length = 0;
    std::atomic<int32_t> status{0};  // 0 open, 1 confirmed, 2 cancelled
    // Asynchronous: the main thread must not wait for this thread, and the
    // strings stay alive because this thread waits for the answer.
    MAIN_THREAD_ASYNC_EM_ASM(
        {
            const str = (p) => UTF8ToString(p);
            // One declaration per statement: a top-level comma would split the macro argument.
            const header = str($0);
            const sub = str($1);
            const guide = str($2);
            const initial = str($3);
            const message = str($4);
            const minLen = $5;
            const maxLen = $6;
            const password = $7;
            const buf = $8;
            const cap = $9;
            const lenAddr = $10;
            const statusAddr = $11;
            const d = document.createElement('dialog');
            d.className = 'lgp-keyboard';
            d.style.cssText = 'font: 16px system-ui, sans-serif; padding: 16px; border-radius: 8px; min-width: 18em;';
            const form = document.createElement('form');
            form.method = 'dialog';
            const line = (t, css) => {
                if (!t) return;
                const p = document.createElement('p');
                p.textContent = t;
                p.style.cssText = 'margin: 0 0 8px;' + (css || "");
                form.appendChild(p);
            };
            line(header || 'Text', 'font-weight: 600;');
            line(sub);
            line(message, 'color: #b3261e;');
            const input = document.createElement('input');
            input.type = password ? 'password' : 'text';
            input.value = initial;
            input.placeholder = guide;
            input.required = true;
            if (minLen > 0) input.minLength = minLen;
            if (maxLen > 0) input.maxLength = maxLen;
            input.autocomplete = 'off';
            input.style.cssText = 'width: 100%; box-sizing: border-box; font: inherit; padding: 4px;';
            form.appendChild(input);
            const buttons = document.createElement('p');
            buttons.style.cssText = 'margin: 12px 0 0; text-align: right;';
            // OK is the form's only submit button, so Enter confirms; Cancel
            // and Escape close the dialog without a value.
            const button = (label) => {
                const b = document.createElement('button');
                b.textContent = label;
                b.style.marginLeft = '8px';
                buttons.appendChild(b);
                return b;
            };
            const cancel = button('Cancel');
            cancel.type = 'button';
            cancel.addEventListener('click', () => d.close('cancel'));
            button('OK').value = 'ok';
            form.appendChild(buttons);
            d.appendChild(form);
            // Refresh the heap views after memory growth (the helper exists only in some builds).
            const fresh = () => { if (typeof growMemViews === 'function') growMemViews(); };
            d.addEventListener('close', () => {
                fresh();
                const ok = d.returnValue === 'ok';
                if (ok) {
                    const bytes = new TextEncoder().encode(input.value);
                    const n = Math.min(bytes.length, cap);
                    HEAPU8.set(bytes.subarray(0, n), buf);
                    HEAP32[lenAddr >> 2] = n;
                }
                const heap = HEAP32;
                Atomics.store(heap, statusAddr >> 2, ok ? 1 : 2);
                Atomics.notify(heap, statusAddr >> 2);
                d.remove();
            });
            document.body.appendChild(d);
            d.showModal();
            input.focus();
            input.select();
        },
        request.header.c_str(), request.sub.c_str(), request.guide.c_str(), request.initial.c_str(),
        request.message.c_str(), request.min_length, request.max_length, request.password ? 1 : 0, buffer.data(),
        static_cast<int32_t>(buffer.size()), &length, &status);
    while (status.load() == 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (status.load() != 1)
        return false;
    text->assign(buffer.data(), static_cast<size_t>(length));
    return true;
}

}  // namespace lgp::platform::web
