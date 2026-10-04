# AI reading assistant

Open a document and choose **View → AI Reading Assistant**. Add a model in **Models…**, then ask a question about the current page. The **Ask** button is inside the prompt field and changes to **Cancel** while a request is running. Canceling restores the question for another try. For a selected passage, right-click it and choose **Ask AI about Selected Text**; the panel opens with the selection as context, and waits for a question.

Each question sends the current page's extracted text when available. Vision profiles also send a JPEG rendering of the current page. Page content is sent only after the reader presses **Ask**. The assistant may use its own knowledge to answer; it is not limited to passages in the document. The first version does not search the whole book.

## Model profiles

- **OpenAI-compatible Chat Completions:** base URL usually ends in `/v1`; requires a model name and API key. History is replayed from Okular's local record.
- **Anthropic-compatible Messages:** base URL normally points to the service root; requires a model name and API key. History is replayed from Okular's local record.
- **OpenAI Responses:** uses `/v1/conversations` and `/v1/responses` so the service keeps conversation state. Requires a supporting endpoint, model and API key.
- **Local Codex CLI:** uses the installed, signed-in `codex` command and resumes the saved Codex session ID. No API key is entered in Okular. The Codex process runs with a read-only sandbox.

Each model profile has an **Extra arguments** field. For Codex, enter CLI options such as `-c model_reasoning_effort=medium`. Okular uses `low` reasoning effort by default for new and resumed Codex turns; an explicit value in this field overrides it. For HTTP providers, enter a JSON object of additional request fields: for example `{"reasoning_effort":"low"}` for Chat Completions, `{"reasoning":{"effort":"low"}}` for Responses, or `{"max_tokens":512}` for Anthropic. Okular keeps the model, conversation and message fields under its own control.

Mark **This model accepts page images** only if that model supports image input. Text-only profiles require selected text to ask about a page.

API keys are saved in KWallet when available; otherwise they remain in memory for this Okular process. Named profiles and conversations are local. Conversations are separated by document content hash and profile ID. The local conversation record contains instructions, questions, answers, selected text, extracted page text and the most recent page image. When a new question is sent through a stateless API, earlier page images are discarded while earlier text stays in the request history. Responses and Codex store a remote session ID as well; their providers keep the full conversation.

To start a fresh conversation for the current document and model, open the arrow menu on **Models…** and choose **Start new conversation**. Okular removes its local conversation record and starts a new provider session on the next question. Saved annotations and the provider's earlier session history remain available outside Okular.

Use **Models… → Conversation instructions…** to set a basic prompt for the current document and model conversation, such as “Answer in Chinese with a patient tone.” Okular saves it with the conversation and sends it on every turn, including resumed Codex and Responses sessions. Editing it affects future answers. Starting a new conversation clears it.

## Answers and annotations

Answers render offline with Markdown and KaTeX math (`$…$`, `$$…$$`, `\(…\)`, `\[…\]`). Raw HTML and Mermaid are not supported. Remote images are shown as links; external links open in the browser.

Choose **Save as annotation** under an answer and click a point on the answer's PDF page. Okular creates a note containing the question and answer and saves it to the local PDF annotation sidecar. The original PDF bytes remain unchanged. This action is available for local PDFs only. See [annotation-sidecars.md](annotation-sidecars.md) for the sidecar format and location.

## Mobile app

Open **AI Reading Assistant** from the document toolbar or main menu. To use a
passage as context, long-press text, adjust the selection handles and tap
**Ask AI**. This keeps the reading position and uses the selected passage's page.
You can configure models before opening a document; asking questions requires
an open document.

Use **Models… → Add model…** to configure an OpenAI-compatible Chat Completions,
OpenAI Responses or Anthropic-compatible Messages profile. Local Codex CLI is
available in the desktop assistant only. Model settings and local conversations
use the same storage format as the desktop assistant. On Android, where KWallet
is unavailable, API keys stay in memory and must be entered again after restart.

To use an eligible **ChatGPT Plus or Pro subscription**, choose **Models… →
Continue with ChatGPT…**, then **Continue with ChatGPT**. Okular opens the system
browser for sign-in and permission to use your ChatGPT plan. Return to Okular,
choose a model from your account's available models, and press **Save**. This also
works before opening a document. Use **Add account…** for another account or
workspace and **Sign out** to revoke that saved session. **Manage usage** opens
ChatGPT's settings; requests share your existing plan limits. OpenAI API keys
remain a separate, usage-billed option.

The browser returns to a temporary listener inside Okular at
`http://127.0.0.1:<port>/auth/callback`; no separate server is needed on your
phone. Android shows a sign-in notification while Okular waits for this callback
and retrieves your account's models. Tap it to return to Okular. The service
stops when sign-in finishes, is canceled, or times out.

ChatGPT authorization is independent of the desktop Codex CLI login. Credentials
are stored atomically in the app's private data directory with owner-only
permissions, excluded from Android backup and device transfer, and refreshed
before expiry. They are never exposed to QML or saved in model profiles or
conversation records. The mobile build uses optional OpenSSL for ID-token
signature verification; builds without it keep the API-key providers available.

ChatGPT plan requests use the public Responses endpoint with `store: false` and
`stream: true`, replaying local text history and the current page image rather
than creating a remote conversation. Answers appear while streaming; canceled
or incomplete responses are discarded so the question can be retried. See the
[OpenAI sign-in documentation](https://developers.openai.com/siwc/token-sharing-open-source).

Press **Ask** to send the current page's text and, for vision profiles, its image.
Text-only profiles work with extracted page text or selected text. Rendering and
image encoding are asynchronous. **Cancel** restores the question and previous
conversation so you can retry. **Models… → Start new conversation** removes the
local conversation for this document and model after confirmation.

Mobile conversations use the document content hash when its file descriptor can
be rewound. For streaming Android providers, the original content URI identifies
the book. Reopening that URI with the same model restores its local conversation.
A bare file descriptor without a source URI starts a separate conversation each
time it is opened, since descriptor numbers can be reused for other documents.

Mobile answers render offline as selectable Markdown. Images become text or
HTTP/HTTPS links instead of loading resources, and raw HTML is discarded.
HTTP/HTTPS links open in the browser when tapped. Questions remain plain text.
The mobile assistant does not yet provide the desktop assistant's KaTeX
rendering, conversation instruction editor or save-answer-as-annotation action.
