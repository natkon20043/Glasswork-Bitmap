#include "MainWindow.hpp"

#include "App.hpp"
#include "CssColor.hpp"
#include "Fuzzy.hpp"
#include "JsonText.hpp"
#include "LanguageServer.hpp"
#include "Loop.hpp"
#include "Paths.hpp"
#include "Platform.hpp"
#include "Services.hpp"
#include "Snippet.hpp"
#include "WordSuggestions.hpp"

#include <algorithm>

namespace gw::app {

namespace {

/// The text of a hover answer: a string, a marked string, a list of them, or MarkupContent.
std::string markdownFromHover(const Json& contents) {
    auto fromOne = [](const Json& piece) -> std::string {
        if (piece.is_string()) {
            return piece.get<std::string>();
        }
        if (piece.is_object() && piece.contains("value") && piece["value"].is_string()) {
            std::string value = piece["value"].get<std::string>();
            if (piece.contains("language") && piece["language"].is_string()) {
                return "```" + piece["language"].get<std::string>() + "\n" + value + "\n```";
            }
            return value;
        }
        return std::string();
    };
    std::string result;
    if (contents.is_array()) {
        for (const Json& piece : contents) {
            std::string text = fromOne(piece);
            if (!text.empty()) {
                result += (result.empty() ? "" : "\n\n") + text;
            }
        }
    } else {
        result = fromOne(contents);
    }
    return result;
}

std::string markupText(const Json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_object() && value.contains("value") && value["value"].is_string()) {
        return value["value"].get<std::string>();
    }
    return std::string();
}

std::string commitCharactersOf(const Json& item) {
    std::string characters;
    if (item.contains("commitCharacters") && item["commitCharacters"].is_array()) {
        for (const Json& character : item["commitCharacters"]) {
            if (character.is_string()) {
                characters += character.get<std::string>();
            }
        }
    }
    return characters;
}

std::vector<std::pair<Range, std::string>> editsFromLsp(const Json& edits, const TextBuffer& buffer) {
    std::vector<std::pair<Range, std::string>> result;
    if (!edits.is_array()) {
        return result;
    }
    for (const Json& edit : edits) {
        try {
            Range range{lsp::position(buffer, edit.at("range").at("start")), lsp::position(buffer, edit.at("range").at("end"))};
            result.push_back({range, edit.value("newText", "")});
        } catch (const std::exception&) {
        }
    }
    return result;
}

Json commandOfCodeAction(const Json& action) {
    if (action.contains("command") && action["command"].is_object() && action["command"].contains("command")) {
        return action["command"];
    }
    if (action.contains("command") && action["command"].is_string()) {
        return action;
    }
    return Json();
}

Json editInCommandArguments(const Json& action) {
    Json command = commandOfCodeAction(action);
    if (command.is_object() && command.contains("arguments") && command["arguments"].is_array() && !command["arguments"].empty() &&
        command["arguments"][0].is_object()) {
        const Json& first = command["arguments"][0];
        if (first.contains("documentChanges") || first.contains("changes")) {
            return first;
        }
    }
    return Json();
}

struct SymbolRow {
    std::string name;
    std::string container;
    int kind = 0;
    Json location;
};

void collectSymbols(const Json& symbols, const std::string& container, const std::string& uri, std::vector<SymbolRow>& rows) {
    if (!symbols.is_array()) {
        return;
    }
    for (const Json& symbol : symbols) {
        if (!symbol.is_object()) {
            continue;
        }
        SymbolRow row;
        row.name = symbol.value("name", "");
        row.kind = symbol.value("kind", 0);
        if (row.name.empty()) {
            continue;
        }
        if (symbol.contains("selectionRange")) {
            row.container = container;
            row.location = {{"uri", uri}, {"range", symbol["selectionRange"]}};
            rows.push_back(row);
            if (symbol.contains("children")) {
                collectSymbols(symbol["children"], container.empty() ? row.name : container + "::" + row.name, uri, rows);
            }
        } else if (symbol.contains("location")) {
            row.container = symbol.value("containerName", "");
            row.location = symbol["location"];
            rows.push_back(row);
        }
    }
}

const char* symbolKindName(int kind) {
    switch (kind) {
        case 2: return "module";
        case 3: return "namespace";
        case 4: return "package";
        case 5: return "class";
        case 6: return "method";
        case 7: return "property";
        case 8: return "field";
        case 9: return "constructor";
        case 10: return "enum";
        case 11: return "interface";
        case 12: return "function";
        case 13: return "variable";
        case 14: return "constant";
        case 22: return "enum member";
        case 23: return "struct";
        case 26: return "type parameter";
        default: return "";
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Setting up

void MainWindow::setUpLanguage() {
    language_ = std::make_unique<LanguageService>();
    language_->enabled = Services::shared().settings().languageServers;
    extensionHost_ = std::make_unique<ExtensionHostService>(*this);
    language_->extensionProvider = extensionHost_.get();
    language_->settingsDidChange(extensionHost_->allSettings());
    language_->onServerMessage = [this](const std::string& server, int type, const std::string& message) {
        extensionHost_->appendLog("[" + std::to_string(type) + "] " + message + "\n", server);
        if (type <= 3) {
            hostShowStatusMessage(server + ": " + message.substr(0, message.find('\n')), 10);
        }
    };
    language_->onServerLog = [this](const std::string& server, const std::string& message) {
        extensionHost_->appendLog(message + "\n", server);
    };
    language_->onServerProgress = [this](const std::string& server, const std::string& token, const std::string& kind,
                                         const std::string& title, const std::string& message, int percentage) {
        serverProgress(server, token, kind, title, message, percentage);
    };
    language_->onServerQuestion = [this](const std::string& server, const std::string& message, const std::vector<std::string>& actions,
                                         std::function<void(const std::string&)> reply) {
        DialogOverlay::Options options;
        options.title = server;
        options.lines = {message};
        options.buttons = actions.empty() ? std::vector<std::string>{"OK"} : actions;
        if (options.buttons.size() > 4) {
            options.buttons.resize(4);
        }
        std::vector<std::string> choices = actions;
        pushOverlay(std::make_unique<DialogOverlay>(options, [choices, reply](int button, const std::string&) {
            reply(button >= 0 && static_cast<size_t>(button) < choices.size() ? choices[static_cast<size_t>(button)] : std::string());
        }));
    };
    language_->onApplyEdit = [this](const Json& edit, std::function<void(bool)> reply) { reply(applyWorkspaceEdit(edit)); };
    language_->onDiagnosticsChanged = [this](Document* document) { diagnosticsChanged(document); };
    language_->onStatusChanged = [this] {
        updateStatus();
        scheduleEditorStateForExtensions();
    };
    language_->onInlayHintsChanged = [this](Document* document) {
        if (Tab* tab = currentTab(); tab && tab->document.get() == document) {
            editor_.setInlayHints(language_->inlayHintsFor(document));
        }
    };
    language_->onSemanticTokensChanged = [this](Document* document) {
        if (Tab* tab = currentTab(); tab && tab->document.get() == document) {
            editor_.setSemanticTokens(language_->semanticTokensFor(document));
        }
    };
}

void MainWindow::diagnosticsChanged(Document* document) {
    Tab* tab = currentTab();
    if (tab && tab->document.get() == document) {
        editor_.setDiagnostics(language_->diagnosticsFor(document));
        if (panelOpen_ && panel_ == SidePanel::Issues) {
            reloadProblems();
        }
    }
    updateStatus();
}

void MainWindow::serverProgress(const std::string& server, const std::string& token, const std::string& kind, const std::string& title,
                                const std::string& message, int percentage) {
    std::string key = server + "/" + token;
    if (kind == "end") {
        serverProgress_.erase(key);
    } else {
        std::string heading = title;
        if (heading.empty()) {
            auto previous = serverProgress_.find(key);
            if (previous != serverProgress_.end()) {
                std::string text = previous->second.substr(server.size() + 2);
                heading = text.substr(0, text.find("  "));
            }
        }
        std::string line = server + ": " + (heading.empty() ? "Working" : heading);
        if (!message.empty()) {
            line += "  " + message;
        }
        if (percentage >= 0) {
            line += " " + std::to_string(percentage) + "%";
        }
        serverProgress_[key] = line;
    }
    updateStatus();
}

// ---------------------------------------------------------------------------
// Hover and signatures

void MainWindow::hoverAt(Position position, Range word) {
    Tab* tab = currentTab();
    if (!tab || tab->kind != Tab::Kind::Text || tab->path().empty() || topOverlay()) {
        return;
    }
    Document* document = tab->document.get();
    uint64_t token = ++hoverToken_;
    std::vector<std::string> problems;
    for (const Diagnostic& diagnostic : language_->diagnosticsFor(document)) {
        bool sameLine = diagnostic.range.start.line == position.line;
        bool overlaps = diagnostic.range.start.column <= word.end.column && diagnostic.range.end.column >= word.start.column;
        if (sameLine && overlaps) {
            problems.push_back(std::string(diagnostic.severity == 1 ? "Error: " : "Warning: ") + diagnostic.message);
        }
    }
    std::weak_ptr<bool> alive = alive_;
    auto show = [this, alive, token, word, problems](const std::string& markdown) {
        if (alive.expired() || token != hoverToken_ || topOverlay()) {
            return;
        }
        ui::Rect rect = editor_.rectForPosition(word.start).unite(editor_.rectForPosition(word.end));
        signatureLine_ = -1;
        hover_.show(markdown, problems, rect, canvas_.bounds());
        dirty_ = true;
    };
    if (!language_->serverSupports("hoverProvider", document)) {
        if (!problems.empty()) {
            show("");
        }
        return;
    }
    language_->request("textDocument/hover", language_->positionParams(document, position), document, true,
                       [show, problems](const Json& result, const std::string& error) {
        std::string markdown;
        if (error.empty() && result.is_object() && result.contains("contents")) {
            markdown = markdownFromHover(result["contents"]);
        }
        if (!markdown.empty() || !problems.empty()) {
            show(markdown);
        }
    });
}

void MainWindow::hideHint() {
    if (hover_.visible()) {
        hover_.hide();
        dirty_ = true;
    }
    signatureLine_ = -1;
}

void MainWindow::requestSignatureHelp() {
    Tab* tab = currentTab();
    if (!tab || tab->kind != Tab::Kind::Text || tab->path().empty()) {
        return;
    }
    Document* document = tab->document.get();
    if (!language_->serverSupports("signatureHelpProvider", document)) {
        return;
    }
    Position caret = document->selection.caret;
    uint64_t token = ++hoverToken_;
    std::weak_ptr<bool> alive = alive_;
    language_->request("textDocument/signatureHelp", language_->positionParams(document, caret), document, true,
                       [this, alive, token, caret](const Json& result, const std::string& error) {
        if (alive.expired() || !error.empty() || token != hoverToken_) {
            return;
        }
        if (!result.is_object() || !result.contains("signatures") || !result["signatures"].is_array() || result["signatures"].empty()) {
            return;
        }
        size_t active = result.value("activeSignature", 0);
        const Json& signatures = result["signatures"];
        const Json& signature = signatures[std::min(active, signatures.size() - 1)];
        std::string label = signature.value("label", "");
        int parameterIndex = signature.contains("activeParameter") && signature["activeParameter"].is_number()
                                 ? signature["activeParameter"].get<int>()
                                 : result.value("activeParameter", 0);
        int activeStart = -1, activeEnd = -1;
        std::string documentation;
        if (signature.contains("parameters") && signature["parameters"].is_array() && parameterIndex >= 0 &&
            static_cast<size_t>(parameterIndex) < signature["parameters"].size()) {
            const Json& parameter = signature["parameters"][static_cast<size_t>(parameterIndex)];
            if (parameter.contains("label") && parameter["label"].is_string()) {
                std::string name = parameter["label"].get<std::string>();
                size_t found = label.find(name);
                if (found != std::string::npos) {
                    activeStart = static_cast<int>(found);
                    activeEnd = static_cast<int>(found + name.size());
                }
            } else if (parameter.contains("label") && parameter["label"].is_array() && parameter["label"].size() == 2) {
                size_t start = parameter["label"][0].get<size_t>(), end = parameter["label"][1].get<size_t>();
                if (start <= end && end <= label.size()) {
                    activeStart = static_cast<int>(start);
                    activeEnd = static_cast<int>(end);
                }
            }
            if (parameter.contains("documentation")) {
                documentation = markupText(parameter["documentation"]);
            }
        }
        if (documentation.empty() && signature.contains("documentation")) {
            documentation = markupText(signature["documentation"]);
        }
        hover_.showSignature(label, activeStart, activeEnd, documentation, editor_.rectForPosition(caret), canvas_.bounds());
        signatureLine_ = static_cast<long>(caret.line);
        dirty_ = true;
    });
}

// ---------------------------------------------------------------------------
// Completion

Position MainWindow::wordStartBeforeCaret() const {
    Tab* tab = currentTab();
    Position caret = tab->document->selection.caret;
    std::string_view line = tab->document->buffer().line(caret.line);
    size_t column = std::min(caret.column, line.size());
    while (column > 0 && isWordByte(static_cast<unsigned char>(line[column - 1]))) {
        --column;
    }
    return {caret.line, column};
}

std::string MainWindow::typedPrefix() const {
    Tab* tab = currentTab();
    Position caret = tab->document->selection.caret;
    Position start = wordStartBeforeCaret();
    if (start.line != caret.line || start.column >= caret.column) {
        return std::string();
    }
    return tab->document->buffer().text({start, caret});
}

bool MainWindow::caretInWordStartingAt(Position start) const {
    Tab* tab = currentTab();
    if (!tab || tab->kind != Tab::Kind::Text) {
        return false;
    }
    const Document& document = *tab->document;
    Position caret = document.selection.caret;
    if (!document.selection.empty() || caret.line != start.line || caret.column < start.column) {
        return false;
    }
    std::string_view line = document.buffer().line(caret.line);
    if (caret.column > line.size()) {
        return false;
    }
    for (size_t column = start.column; column < caret.column; ++column) {
        if (!isWordByte(static_cast<unsigned char>(line[column]))) {
            return false;
        }
    }
    return true;
}

void MainWindow::editorTypedText(const std::optional<std::string>& typed) {
    Tab* tab = currentTab();
    if (!tab || tab->kind != Tab::Kind::Text) {
        return;
    }
    if (!typed) {
        if (suggestions_.visible()) {
            if (caretInWordStartingAt(suggestionWordStart_)) {
                suggestions_.filter(typedPrefix());
            } else {
                suggestions_.hide();
                ++suggestionToken_;
            }
        }
        return;
    }
    const std::string& text = *typed;
    char last = text.empty() ? 0 : text.back();
    if (last == '(' || last == ',') {
        requestSignatureHelp();
    } else if (last == ')' || last == ';') {
        hideHint();
    } else if (signatureLine_ < 0) {
        hideHint();
    }
    bool wordCharacter = (last >= 'a' && last <= 'z') || (last >= 'A' && last <= 'Z') || (last >= '0' && last <= '9') || last == '_';
    std::string triggers = language_->completionTriggerCharacters(tab->document.get());
    bool triggerCharacter = text.size() == 1 && triggers.find(text) != std::string::npos;

    std::shared_ptr<Suggestion> highlighted = suggestions_.selected();
    if (suggestions_.visible() && !wordCharacter && highlighted && text.size() == 1 &&
        highlighted->commitCharacters.find(text) != std::string::npos) {
        acceptSuggestion(highlighted, text);
        return;
    }
    if (suggestions_.visible() && wordCharacter) {
        suggestions_.filter(typedPrefix());
        if (suggestionsIncomplete_) {
            uint64_t token = suggestionToken_;
            std::weak_ptr<bool> alive = alive_;
            afterDelay(0.05, [this, alive, token] {
                if (!alive.expired() && token == suggestionToken_ && suggestions_.visible()) {
                    requestSuggestions(false);
                }
            });
        }
        dirty_ = true;
        return;
    }
    suggestions_.hide();
    ++suggestionToken_;
    if (triggerCharacter || (wordCharacter && !typedPrefix().empty())) {
        uint64_t token = ++suggestionToken_;
        std::weak_ptr<bool> alive = alive_;
        afterDelay(0.09, [this, alive, token] {
            if (!alive.expired() && token == suggestionToken_) {
                requestSuggestions(false);
            }
        });
    }
}

void MainWindow::requestSuggestions(bool explicitly) {
    Tab* tab = currentTab();
    if (!tab || tab->kind != Tab::Kind::Text) {
        return;
    }
    Document* document = tab->document.get();
    if (tab->path().empty() || !language_->enabled || !language_->serverSupports("completionProvider", document)) {
        showWordSuggestions(wordStartBeforeCaret(), explicitly);
        return;
    }
    Position caret = document->selection.caret;
    Position wordStart = wordStartBeforeCaret();
    uint64_t token = suggestionToken_;
    Json params = language_->positionParams(document, caret);
    int triggerKind = 1;
    std::string triggerCharacter;
    if (!explicitly && caret.column > 0) {
        std::string before = document->buffer().text({{caret.line, caret.column - 1}, caret});
        std::string triggers = language_->completionTriggerCharacters(document);
        if (before.size() == 1 && triggers.find(before) != std::string::npos) {
            triggerKind = 2;
            triggerCharacter = before;
        } else if (suggestions_.visible() && suggestionsIncomplete_) {
            triggerKind = 3;
        }
    }
    params["context"] = {{"triggerKind", triggerKind}};
    if (!triggerCharacter.empty()) {
        params["context"]["triggerCharacter"] = triggerCharacter;
    }
    std::weak_ptr<bool> alive = alive_;
    language_->request("textDocument/completion", params, document, true,
                       [this, alive, token, explicitly, wordStart](const Json& result, const std::string& error) {
        if (alive.expired() || !error.empty() || (!explicitly && token != suggestionToken_)) {
            return;
        }
        showSuggestions(result, wordStart);
    });
}

std::vector<std::shared_ptr<Suggestion>> MainWindow::snippetSuggestions() {
    std::vector<std::shared_ptr<Suggestion>> items;
    Tab* tab = currentTab();
    if (!tab || tab->kind != Tab::Kind::Text) {
        return items;
    }
    for (const SnippetDefinition& definition : Services::shared().extensions().snippets().forLanguage(tab->document->languageId)) {
        auto item = std::make_shared<Suggestion>();
        item->label = definition.prefix;
        item->detail = definition.description;
        item->filterText = item->label;
        item->sortText = "9" + item->label;
        item->kind = 15;
        item->documentation = "```\n" + definition.body + "\n```";
        item->raw = std::make_shared<Json>(Json{{"label", definition.prefix},
                                                {"detail", definition.description},
                                                {"insertText", definition.body},
                                                {"insertTextFormat", 2},
                                                {"kind", 15}});
        item->resolved = true;
        items.push_back(item);
    }
    return items;
}

void MainWindow::showSuggestions(const Json& result, Position wordStart) {
    if (!editor_.insertsText()) {
        return;
    }
    Json rows = result;
    suggestionsIncomplete_ = result.is_object() && result.value("isIncomplete", false);
    if (result.is_object()) {
        rows = result.value("items", Json::array());
    }
    if (!rows.is_array() || rows.empty()) {
        showWordSuggestions(wordStart, false);
        return;
    }
    std::vector<std::shared_ptr<Suggestion>> items;
    for (const Json& row : rows) {
        if (!row.is_object()) {
            continue;
        }
        std::string label = row.value("label", "");
        if (label.empty()) {
            continue;
        }
        auto item = std::make_shared<Suggestion>();
        item->label = label;
        std::string detail = row.value("detail", "");
        if (detail.empty() && row.contains("labelDetails") && row["labelDetails"].is_object()) {
            detail = row["labelDetails"].value("description", "");
        }
        item->detail = detail;
        item->filterText = row.value("filterText", label);
        item->sortText = row.value("sortText", label);
        item->kind = row.value("kind", 0);
        item->preselect = row.value("preselect", false);
        item->deprecated = row.value("deprecated", false);
        if (row.contains("tags") && row["tags"].is_array()) {
            for (const Json& tag : row["tags"]) {
                if (tag.is_number_integer() && tag.get<int>() == 1) {
                    item->deprecated = true;
                }
            }
        }
        item->documentation = row.contains("documentation") ? markupText(row["documentation"]) : std::string();
        item->commitCharacters = commitCharactersOf(row);
        item->raw = std::make_shared<Json>(row);
        items.push_back(item);
        if (items.size() >= 400) {
            break;
        }
    }
    for (auto& snippet : snippetSuggestions()) {
        items.push_back(snippet);
    }
    if (items.empty()) {
        showWordSuggestions(wordStart, false);
        return;
    }
    presentSuggestions(std::move(items), wordStart);
}

void MainWindow::showWordSuggestions(Position wordStart, bool explicitly) {
    Tab* tab = currentTab();
    if (!tab || tab->kind != Tab::Kind::Text || !editor_.insertsText()) {
        return;
    }
    Document* document = tab->document.get();
    std::string prefix = typedPrefix();
    std::vector<std::shared_ptr<Suggestion>> snippets = snippetSuggestions();
    bool snippetsMatch = false;
    if (!prefix.empty() && prefix.size() < 3 && !explicitly) {
        for (auto& snippet : snippets) {
            if (fuzzyScore(prefix, snippet->filterText)) {
                snippetsMatch = true;
                break;
            }
        }
    }
    if (prefix.size() < 3 && !explicitly && !snippetsMatch) {
        return;
    }
    std::vector<std::string> words = wordSuggestions(document->buffer(), prefix, document->selection.caret);
    std::vector<std::shared_ptr<Suggestion>> items = snippets;
    if (words.empty() && items.empty()) {
        return;
    }
    for (size_t index = 0; index < words.size(); ++index) {
        auto item = std::make_shared<Suggestion>();
        item->label = words[index];
        item->detail = "word in this file";
        item->filterText = item->label;
        char sort[16];
        std::snprintf(sort, sizeof(sort), "%05zu", index);
        item->sortText = sort;
        item->kind = 1;
        item->raw = std::make_shared<Json>(Json{{"label", words[index]}, {"insertText", words[index]}});
        item->resolved = true;
        items.push_back(item);
    }
    presentSuggestions(std::move(items), wordStart);
}

void MainWindow::presentSuggestions(std::vector<std::shared_ptr<Suggestion>> items, Position wordStart) {
    Tab* tab = currentTab();
    if (!tab || !caretInWordStartingAt(wordStart) || topOverlay()) {
        return;
    }
    suggestionWordStart_ = wordStart;
    suggestions_.setItems(std::move(items));
    suggestions_.show(editor_.rectForPosition(tab->document->selection.caret), canvas_.bounds());
    suggestions_.filter(typedPrefix());
    dirty_ = true;
}

void MainWindow::resolveSuggestion(std::shared_ptr<Suggestion> suggestion, Document* document, std::function<void()> then) {
    if (suggestion->resolved || !suggestion->raw || !language_->serverSupports("completionProvider.resolveProvider", document)) {
        then();
        return;
    }
    suggestion->resolved = true;
    std::weak_ptr<bool> alive = alive_;
    language_->request("completionItem/resolve", *suggestion->raw, document, false,
                       [alive, suggestion, then](const Json& result, const std::string& error) {
        if (alive.expired()) {
            return;
        }
        if (error.empty() && result.is_object()) {
            auto merged = std::make_shared<Json>(*suggestion->raw);
            for (const auto& [key, value] : result.items()) {
                (*merged)[key] = value;
            }
            suggestion->raw = merged;
            std::string documentation = result.contains("documentation") ? markupText(result["documentation"]) : std::string();
            if (!documentation.empty()) {
                suggestion->documentation = documentation;
            }
            std::string detail = result.value("detail", "");
            if (!detail.empty()) {
                suggestion->detail = detail;
            }
        }
        then();
    });
}

void MainWindow::acceptSuggestion(std::shared_ptr<Suggestion> suggestion, const std::string& commitCharacter) {
    Tab* tab = currentTab();
    if (!tab || tab->kind != Tab::Kind::Text || !suggestion || !suggestion->raw) {
        return;
    }
    suggestions_.recordAccepted(*suggestion);
    suggestions_.hide();
    ++suggestionToken_;
    if (!commitCharacter.empty()) {
        Position caret = tab->document->selection.caret;
        if (caret.column > 0) {
            editor_.replaceRange({{caret.line, caret.column - 1}, caret}, "");
        }
    }
    Document* document = tab->document.get();
    std::weak_ptr<bool> alive = alive_;
    resolveSuggestion(suggestion, document, [this, alive, suggestion, document, commitCharacter] {
        if (alive.expired()) {
            return;
        }
        Tab* current = currentTab();
        if (!current || current->document.get() != document) {
            return;
        }
        insertSuggestion(*suggestion);
        if (!commitCharacter.empty()) {
            editor_.insertText(commitCharacter);
        }
        dirty_ = true;
    });
}

void MainWindow::insertSuggestion(const Suggestion& suggestion) {
    Tab* tab = currentTab();
    if (!tab || !suggestion.raw) {
        return;
    }
    const Json& item = *suggestion.raw;
    Document* document = tab->document.get();
    const TextBuffer& buffer = document->buffer();
    Position caret = document->selection.caret;
    Range range{wordStartBeforeCaret(), caret};
    std::string text = item.value("insertText", item.value("label", ""));
    try {
        if (item.contains("textEdit") && item["textEdit"].is_object()) {
            const Json& edit = item["textEdit"];
            const Json& editRange = edit.contains("range") ? edit["range"] : edit.at("replace");
            range = {lsp::position(buffer, editRange.at("start")), lsp::position(buffer, editRange.at("end"))};
            text = edit.at("newText").get<std::string>();
        }
    } catch (const std::exception&) {
    }
    bool isSnippet = item.value("insertTextFormat", 1) == 2;
    ParsedSnippet snippet;
    if (isSnippet) {
        std::string_view caretLine = buffer.line(range.start.line);
        size_t indentLength = 0;
        while (indentLength < caretLine.size() && (caretLine[indentLength] == ' ' || caretLine[indentLength] == '\t')) {
            ++indentLength;
        }
        SnippetVariables variables = standardSnippetVariables(tab->path(), folder_);
        variables["TM_SELECTED_TEXT"] = document->selection.empty() ? std::string() : buffer.text(document->selection.range());
        variables["TM_CURRENT_LINE"] = std::string(buffer.line(caret.line));
        variables["TM_LINE_INDEX"] = std::to_string(caret.line);
        variables["TM_LINE_NUMBER"] = std::to_string(caret.line + 1);
        variables["CLIPBOARD"] = clipboardText();
        variables["LINE_COMMENT"] = tab->configuration.lineComment;
        variables["BLOCK_COMMENT_START"] = tab->configuration.blockComment.first;
        variables["BLOCK_COMMENT_END"] = tab->configuration.blockComment.second;
        snippet = parseSnippet(text, caretLine.substr(0, indentLength), &variables);
        text = snippet.text;
    }
    if (range.end < caret && range.end.line == caret.line) {
        range.end = caret;
    }
    std::vector<std::pair<Range, std::string>> edits;
    edits.push_back({range, text});
    if (item.contains("additionalTextEdits") && item["additionalTextEdits"].is_array()) {
        for (auto& extra : editsFromLsp(item["additionalTextEdits"], buffer)) {
            edits.push_back(extra);
        }
    }
    applyEdits(edits, true);
    if (isSnippet && !snippet.hasOnlyFinalStop()) {
        Position snippetStart = range.start;
        for (size_t index = 1; index < edits.size(); ++index) {
            const Range& other = edits[index].first;
            if (other.start < range.start) {
                long removedLines = static_cast<long>(other.end.line) - static_cast<long>(other.start.line);
                long addedLines = std::count(edits[index].second.begin(), edits[index].second.end(), '\n');
                snippetStart.line = static_cast<size_t>(static_cast<long>(snippetStart.line) + addedLines - removedLines);
            }
        }
        tab->document->startSnippet(snippetStart, snippet);
        editor_.selectionDidChange();
    }
}

void MainWindow::applyEdits(std::vector<std::pair<Range, std::string>> edits, bool placeCaret) {
    Tab* tab = currentTab();
    if (!tab || tab->kind != Tab::Kind::Text || tab->document->readOnly || edits.empty()) {
        return;
    }
    Range firstRange = edits.front().first;
    std::string firstText = edits.front().second;
    std::stable_sort(edits.begin(), edits.end(), [](const auto& a, const auto& b) { return b.first.start < a.first.start; });
    editor_.editDocument([&edits](Document& document) {
        for (const auto& [range, replacement] : edits) {
            document.replace(range, replacement);
        }
    });
    if (placeCaret) {
        Position end = endOfInsertion(firstRange.start, firstText);
        for (const auto& [range, replacement] : edits) {
            if (range.start < firstRange.start) {
                size_t removedLines = range.end.line - range.start.line;
                size_t addedLines = static_cast<size_t>(std::count(replacement.begin(), replacement.end(), '\n'));
                if (addedLines >= removedLines) {
                    end.line += addedLines - removedLines;
                } else {
                    end.line -= removedLines - addedLines;
                }
            }
        }
        Position caret = tab->document->buffer().clamp(end);
        tab->document->selection = {caret, caret};
        editor_.jumpToPosition(caret);
    }
}

// ---------------------------------------------------------------------------
// Going places

Document* MainWindow::documentForLanguageCommand() {
    Tab* tab = currentTab();
    if (!tab || tab->kind != Tab::Kind::Text || tab->path().empty()) {
        showMessage("Code Intelligence", "Save this file first so the language server can resolve its project.");
        return nullptr;
    }
    Document* document = tab->document.get();
    if (!language_->hasServerForLanguage(document->languageId)) {
        showMessage("Code Intelligence", "There is no language server for this kind of file yet.");
        return nullptr;
    }
    std::string hint = language_->installHintForLanguage(document->languageId);
    if (!hint.empty()) {
        showMessage("Code Intelligence", hint);
        return nullptr;
    }
    return document;
}

void MainWindow::requestLocations(const std::string& method, const std::string& title, bool openSingle) {
    Document* document = documentForLanguageCommand();
    if (!document) {
        return;
    }
    Json params = language_->positionParams(document, document->selection.caret);
    if (method == "textDocument/references") {
        params["context"] = {{"includeDeclaration", true}};
    }
    setStatusMessage("Asking the language server...", 10);
    std::weak_ptr<bool> alive = alive_;
    language_->request(method, params, document, false, [this, alive, title, openSingle](const Json& result, const std::string& error) {
        if (alive.expired()) {
            return;
        }
        temporaryMessage_.clear();
        if (!error.empty()) {
            showMessage("Code Intelligence", error);
            return;
        }
        showLocations(result, title, openSingle);
    });
}

void MainWindow::showLocations(const Json& result, const std::string& title, bool openSingle) {
    Json rows = result.is_object() ? Json::array({result}) : result;
    std::vector<PaletteItem> items;
    if (rows.is_array()) {
        for (const Json& row : rows) {
            if (!row.is_object()) {
                continue;
            }
            std::string uri = row.value("uri", row.value("targetUri", ""));
            const Json* range = row.contains("targetSelectionRange") ? &row["targetSelectionRange"] : row.contains("range") ? &row["range"] : nullptr;
            if (!range || uri.empty()) {
                continue;
            }
            std::string path = lsp::pathFromURI(uri);
            size_t line = (*range)["start"].value("line", 0) + 1;
            PaletteItem item;
            item.title = utf8FromPath(pathFromUtf8(path).filename());
            item.subtitle = relativePath(path) + " - line " + std::to_string(line);
            Json location = row;
            item.run = [this, location] { openLocation(location); };
            items.push_back(std::move(item));
        }
    }
    if (items.empty()) {
        showMessage("Code Intelligence", "No results at this position.");
        return;
    }
    if (openSingle && items.size() == 1) {
        items.front().run();
        return;
    }
    showPaletteItems(std::move(items), title);
}

void MainWindow::showPaletteItems(std::vector<PaletteItem> items, const std::string& placeholder) {
    pushOverlay(std::make_unique<PaletteOverlay>(placeholder, [items](const std::string& query) {
        std::vector<PaletteItem> filtered;
        for (PaletteItem item : items) {
            if (query.empty() || paletteMatch(query, item, nullptr)) {
                filtered.push_back(std::move(item));
            } else {
                PaletteItem bySubtitle = item;
                std::string lower = item.subtitle, wanted = query;
                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                std::transform(wanted.begin(), wanted.end(), wanted.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (lower.find(wanted) != std::string::npos) {
                    filtered.push_back(std::move(bySubtitle));
                }
            }
            if (filtered.size() >= 200) {
                break;
            }
        }
        return filtered;
    }));
}

void MainWindow::openLocation(const Json& location) {
    try {
        std::string uri = location.value("uri", location.value("targetUri", ""));
        std::string path = lsp::pathFromURI(uri);
        if (path.empty()) {
            return;
        }
        rememberLocation();
        openPath(path);
        Tab* tab = currentTab();
        if (!tab || tab->kind != Tab::Kind::Text) {
            return;
        }
        const Json& range = location.contains("targetSelectionRange") ? location.at("targetSelectionRange") : location.at("range");
        editor_.jumpToPosition(lsp::position(tab->document->buffer(), range.at("start")));
        setFocus(Focus::Editor);
    } catch (const std::exception&) {
        showMessage("Code Intelligence", "The language server returned an invalid source location.");
    }
}

void MainWindow::showProblemsList() {
    Tab* tab = currentTab();
    if (!tab || tab->kind != Tab::Kind::Text) {
        return;
    }
    const std::vector<Diagnostic>& diagnostics = language_->diagnosticsFor(tab->document.get());
    if (diagnostics.empty()) {
        std::string hint = language_->installHintForLanguage(tab->document->languageId);
        std::string status = language_->statusTextForLanguage(tab->document->languageId);
        showMessage("Problems", !hint.empty() ? hint : status.empty() ? "There is no language server for this kind of file yet."
                                                                      : "No problems found in this file.");
        return;
    }
    std::vector<PaletteItem> items;
    for (const Diagnostic& diagnostic : diagnostics) {
        PaletteItem item;
        item.title = diagnostic.message.substr(0, diagnostic.message.find('\n'));
        item.subtitle = std::string(diagnostic.severity == 1 ? "error" : diagnostic.severity == 2 ? "warning" : "note") + " - line " +
                        std::to_string(diagnostic.range.start.line + 1);
        Position where = diagnostic.range.start;
        item.run = [this, where] {
            rememberLocation();
            editor_.jumpToPosition(where);
            setFocus(Focus::Editor);
        };
        items.push_back(std::move(item));
    }
    showPaletteItems(std::move(items), "Problems in this file...");
}

// ---------------------------------------------------------------------------
// Quick fixes, rename, format, symbols

void MainWindow::showQuickFixes() {
    Document* document = documentForLanguageCommand();
    if (!document) {
        return;
    }
    if (!language_->serverSupports("codeActionProvider", document)) {
        showMessage("Code Intelligence", "The language server for this file doesn't offer quick fixes.");
        return;
    }
    Range range = document->selection.range();
    const TextBuffer& buffer = document->buffer();
    Json diagnostics = Json::array();
    for (const Diagnostic& diagnostic : language_->diagnosticsFor(document)) {
        bool touches = diagnostic.range.start.line <= range.end.line && diagnostic.range.end.line >= range.start.line;
        if (touches && diagnostic.raw) {
            diagnostics.push_back(*diagnostic.raw);
        }
    }
    Json params = {
        {"textDocument", {{"uri", lsp::fileURI(utf8FromPath(*document->path()))}}},
        {"range", {{"start", lsp::position(buffer, range.start)}, {"end", lsp::position(buffer, range.end)}}},
        {"context", {{"diagnostics", diagnostics}}},
    };
    std::weak_ptr<bool> alive = alive_;
    language_->request("textDocument/codeAction", params, document, true, [this, alive, document](const Json& result, const std::string& error) {
        if (alive.expired()) {
            return;
        }
        if (!error.empty()) {
            showMessage("Code Intelligence", error);
            return;
        }
        std::vector<PaletteItem> items;
        if (result.is_array()) {
            for (const Json& action : result) {
                if (!action.is_object() || !codeActionCanRun(action, document)) {
                    continue;
                }
                PaletteItem item;
                item.title = action.value("title", "Fix");
                std::string kind = action.value("kind", "");
                item.subtitle = kind.rfind("refactor", 0) == 0 ? "refactor" : action.value("isPreferred", false) ? "preferred fix" : "";
                Json copy = action;
                item.run = [this, copy, document] { runCodeAction(copy, document, false); };
                items.push_back(std::move(item));
            }
        }
        if (items.empty()) {
            showMessage("Code Intelligence", "There are no quick fixes here.");
            return;
        }
        if (items.size() == 1) {
            items.front().run();
            return;
        }
        showPaletteItems(std::move(items), "Quick fixes...");
    });
}

bool MainWindow::codeActionCanRun(const Json& action, Document* document) {
    if (action.contains("edit") && action["edit"].is_object()) {
        return true;
    }
    if (!commandOfCodeAction(action).is_null()) {
        return true;
    }
    return action.contains("data") && language_->serverSupports("codeActionProvider.resolveProvider", document);
}

void MainWindow::runCodeAction(const Json& action, Document* document, bool resolved) {
    bool hasEdit = action.contains("edit") && action["edit"].is_object();
    Json command = commandOfCodeAction(action);
    Json legacyEdit = editInCommandArguments(action);
    if (hasEdit) {
        applyWorkspaceEdit(action["edit"]);
    } else if (!legacyEdit.is_null()) {
        applyWorkspaceEdit(legacyEdit);
        return;
    }
    if (!command.is_null()) {
        if (action.contains("__gwServer") && !command.contains("__gwServer")) {
            command["__gwServer"] = action["__gwServer"];
        }
        executeServerCommand(command, document);
        return;
    }
    if (hasEdit) {
        return;
    }
    if (resolved) {
        showMessage("Code Intelligence", "The language server had nothing to do for that.");
        return;
    }
    std::weak_ptr<bool> alive = alive_;
    language_->request("codeAction/resolve", action, document, true, [this, alive, document](const Json& result, const std::string& error) {
        if (alive.expired()) {
            return;
        }
        if (!error.empty()) {
            showMessage("Code Intelligence", error);
            return;
        }
        runCodeAction(result, document, true);
    });
}

void MainWindow::executeServerCommand(const Json& command, Document* document) {
    Json params = {
        {"command", command.value("command", "")},
        {"arguments", command.contains("arguments") ? command["arguments"] : Json::array()},
    };
    if (command.contains("__gwServer")) {
        params["__gwServer"] = command["__gwServer"];
    }
    setStatusMessage("Running...", 10);
    std::weak_ptr<bool> alive = alive_;
    language_->request("workspace/executeCommand", params, document, false, [this, alive](const Json&, const std::string& error) {
        if (alive.expired()) {
            return;
        }
        temporaryMessage_.clear();
        if (!error.empty()) {
            showMessage("Code Intelligence", error);
        }
        dirty_ = true;
    });
}

bool MainWindow::applyWorkspaceEdit(const Json& edit) {
    std::vector<std::pair<std::string, Json>> fileEdits;
    if (edit.is_object() && edit.contains("documentChanges") && edit["documentChanges"].is_array()) {
        for (const Json& change : edit["documentChanges"]) {
            if (change.contains("textDocument") && change.contains("edits")) {
                fileEdits.push_back({change["textDocument"].value("uri", ""), change["edits"]});
            }
        }
    } else if (edit.is_object() && edit.contains("changes") && edit["changes"].is_object()) {
        for (const auto& [uri, edits] : edit["changes"].items()) {
            fileEdits.push_back({uri, edits});
        }
    }
    if (fileEdits.empty()) {
        setStatusMessage("The language server found nothing to change.");
        return false;
    }
    size_t changedFiles = 0, totalEdits = 0;
    std::vector<std::string> failures;
    for (const auto& [uri, edits] : fileEdits) {
        std::string path = lsp::pathFromURI(uri);
        if (path.empty()) {
            continue;
        }
        totalEdits += edits.size();
        Tab* openTab = tabForPath(path);
        if (openTab && openTab->kind == Tab::Kind::Text) {
            auto parsed = editsFromLsp(edits, openTab->document->buffer());
            std::stable_sort(parsed.begin(), parsed.end(), [](const auto& a, const auto& b) { return b.first.start < a.first.start; });
            if (openTab == currentTab()) {
                editor_.editDocument([&parsed](Document& document) {
                    for (const auto& [range, text] : parsed) {
                        document.replace(range, text);
                    }
                });
            } else {
                openTab->document->beginUndoGroup();
                for (const auto& [range, text] : parsed) {
                    openTab->document->replace(range, text);
                }
                openTab->document->endUndoGroup();
                documentEdited(*openTab);
            }
            ++changedFiles;
            continue;
        }
        std::string error;
        auto document = Document::open(pathFromUtf8(path), &error);
        if (!document) {
            failures.push_back(path);
            continue;
        }
        auto parsed = editsFromLsp(edits, document->buffer());
        std::stable_sort(parsed.begin(), parsed.end(), [](const auto& a, const auto& b) { return b.first.start < a.first.start; });
        for (const auto& [range, text] : parsed) {
            document->replace(range, text);
        }
        if (document->save(&error)) {
            ++changedFiles;
        } else {
            failures.push_back(path);
        }
    }
    refresh();
    for (auto& tab : tabs_) {
        refreshGit(*tab);
    }
    if (!failures.empty()) {
        std::string list;
        for (const std::string& failure : failures) {
            list += (list.empty() ? "" : ", ") + abbreviateHome(failure);
        }
        showError("Some files couldn't be changed", list);
        return false;
    }
    setStatusMessage("Changed " + std::to_string(totalEdits) + (totalEdits == 1 ? " place in " : " places in ") +
                     std::to_string(changedFiles) + (changedFiles == 1 ? " file" : " files"));
    return true;
}

void MainWindow::renameSymbol() {
    Document* document = documentForLanguageCommand();
    if (!document) {
        return;
    }
    if (!language_->serverSupports("renameProvider", document)) {
        showMessage("Code Intelligence", "The language server for this file doesn't support renaming.");
        return;
    }
    std::string current = editor_.wordAtCaret();
    if (current.empty()) {
        showMessage("Rename Symbol", "Put the caret on a name to rename it.");
        return;
    }
    Position caret = document->selection.caret;
    DialogOverlay::Options options;
    options.title = "Rename Symbol";
    options.lines = {"Rename \"" + current + "\" everywhere it is used."};
    options.hasField = true;
    options.fieldText = current;
    options.buttons = {"&Rename", "Cancel"};
    std::weak_ptr<bool> alive = alive_;
    pushOverlay(std::make_unique<DialogOverlay>(options, [this, alive, document, caret, current](int button, const std::string& text) {
        std::string name = text;
        while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) {
            name.pop_back();
        }
        while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front()))) {
            name.erase(name.begin());
        }
        if (alive.expired() || button != 0 || name.empty() || name == current) {
            return;
        }
        Json params = language_->positionParams(document, caret);
        params["newName"] = name;
        setStatusMessage("Renaming...", 10);
        language_->request("textDocument/rename", params, document, false, [this, alive](const Json& result, const std::string& error) {
            if (alive.expired()) {
                return;
            }
            temporaryMessage_.clear();
            if (!error.empty()) {
                showMessage("Rename Symbol", error);
                return;
            }
            applyWorkspaceEdit(result);
        });
    }));
}

void MainWindow::formatDocument(Document* document, std::function<void()> after) {
    Json params = {
        {"textDocument", {{"uri", lsp::fileURI(utf8FromPath(*document->path()))}}},
        {"options", {{"tabSize", Services::shared().settings().tabSize}, {"insertSpaces", true}}},
    };
    setStatusMessage("Formatting...", 5);
    std::weak_ptr<bool> alive = alive_;
    language_->request("textDocument/formatting", params, document, true, [this, alive, document, after](const Json& result, const std::string& error) {
        if (alive.expired()) {
            return;
        }
        temporaryMessage_.clear();
        if (!error.empty()) {
            if (!after) {
                showMessage("Format Document", error);
            }
        } else {
            Tab* tab = currentTab();
            if (tab && tab->document.get() == document && result.is_array() && !result.empty()) {
                auto edits = editsFromLsp(result, document->buffer());
                if (!edits.empty()) {
                    Position caret = document->selection.caret;
                    applyEdits(edits, false);
                    editor_.jumpToPosition(document->buffer().clamp(caret));
                }
            }
        }
        if (after) {
            after();
        }
        dirty_ = true;
    });
}

void MainWindow::goToSymbolInFile() {
    Document* document = documentForLanguageCommand();
    if (!document) {
        return;
    }
    if (!language_->serverSupports("documentSymbolProvider", document)) {
        showMessage("Code Intelligence", "The language server for this file can't list its symbols.");
        return;
    }
    std::string uri = lsp::fileURI(utf8FromPath(*document->path()));
    Json params = {{"textDocument", {{"uri", uri}}}};
    std::weak_ptr<bool> alive = alive_;
    language_->request("textDocument/documentSymbol", params, document, false, [this, alive, uri](const Json& result, const std::string& error) {
        if (alive.expired()) {
            return;
        }
        if (!error.empty()) {
            showMessage("Code Intelligence", error);
            return;
        }
        auto rows = std::make_shared<std::vector<SymbolRow>>();
        collectSymbols(result, "", uri, *rows);
        if (rows->empty()) {
            showMessage("Code Intelligence", "No symbols found in this file.");
            return;
        }
        pushOverlay(std::make_unique<PaletteOverlay>("Go to symbol in file...", [this, rows](const std::string& query) {
            struct Ranked {
                int score;
                size_t index;
            };
            std::vector<Ranked> ranked;
            for (size_t index = 0; index < rows->size(); ++index) {
                if (query.empty()) {
                    ranked.push_back({0, index});
                } else if (auto score = fuzzyScore(query, (*rows)[index].name)) {
                    ranked.push_back({*score, index});
                }
            }
            if (!query.empty()) {
                std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) { return a.score > b.score; });
            }
            std::vector<PaletteItem> items;
            for (const Ranked& entry : ranked) {
                const SymbolRow& row = (*rows)[entry.index];
                PaletteItem item;
                item.title = row.name;
                std::string subtitle = symbolKindName(row.kind);
                if (!row.container.empty()) {
                    subtitle += (subtitle.empty() ? "" : " in ") + row.container;
                }
                if (row.location.contains("range")) {
                    subtitle += (subtitle.empty() ? "" : " - ") + std::string("line ") +
                                std::to_string(row.location["range"]["start"].value("line", 0) + 1);
                }
                item.subtitle = subtitle;
                if (!query.empty()) {
                    if (auto match = fuzzyMatch(query, row.name)) {
                        item.matched = match->positions;
                    }
                }
                Json location = row.location;
                item.run = [this, location] { openLocation(location); };
                items.push_back(std::move(item));
                if (items.size() >= 200) {
                    break;
                }
            }
            return items;
        }));
    });
}

void MainWindow::goToSymbolInWorkspace() {
    Document* document = documentForLanguageCommand();
    if (!document) {
        return;
    }
    if (!language_->serverSupports("workspaceSymbolProvider", document)) {
        showMessage("Code Intelligence", "The language server for this file can't search the project's symbols.");
        return;
    }
    struct State {
        std::string requested = "\x01";
        uint64_t token = 0;
        std::vector<SymbolRow> rows;
    };
    auto state = std::make_shared<State>();
    auto holder = std::make_shared<PaletteOverlay*>(nullptr);
    std::weak_ptr<bool> alive = alive_;
    auto provider = [this, alive, state, holder, document](const std::string& query) {
        if (query != state->requested) {
            state->requested = query;
            uint64_t token = ++state->token;
            afterDelay(0.15, [this, alive, state, holder, document, token, query] {
                if (alive.expired() || token != state->token) {
                    return;
                }
                language_->request("workspace/symbol", Json{{"query", query}}, document, false,
                                   [this, alive, state, holder, token](const Json& result, const std::string& error) {
                    if (alive.expired() || !error.empty() || token != state->token) {
                        return;
                    }
                    state->rows.clear();
                    collectSymbols(result, "", "", state->rows);
                    for (auto& overlay : overlays_) {
                        if (overlay.get() == *holder && !overlay->closed()) {
                            (*holder)->refresh();
                            dirty_ = true;
                        }
                    }
                });
            });
        }
        std::vector<PaletteItem> items;
        for (const SymbolRow& row : state->rows) {
            PaletteItem item;
            item.title = row.name;
            std::string subtitle = symbolKindName(row.kind);
            if (!row.container.empty()) {
                subtitle += (subtitle.empty() ? "" : " in ") + row.container;
            }
            std::string path = lsp::pathFromURI(row.location.value("uri", ""));
            if (!path.empty()) {
                subtitle += (subtitle.empty() ? "" : " - ") + relativePath(path);
            }
            item.subtitle = subtitle;
            if (!query.empty()) {
                if (auto match = fuzzyMatch(query, row.name)) {
                    item.matched = match->positions;
                }
            }
            Json location = row.location;
            item.run = [this, location] { openLocation(location); };
            items.push_back(std::move(item));
            if (items.size() >= 200) {
                break;
            }
        }
        return items;
    };
    auto palette = std::make_unique<PaletteOverlay>("Go to symbol in project...", provider);
    *holder = palette.get();
    pushOverlay(std::move(palette));
}

// ---------------------------------------------------------------------------
// Vim's commands

std::string MainWindow::runVimCommand(const std::string& command) {
    size_t letters = 0;
    while (letters < command.size() && std::isalpha(static_cast<unsigned char>(command[letters]))) {
        ++letters;
    }
    std::string name = command.substr(0, letters);
    bool force = letters < command.size() && command[letters] == '!';
    static const std::set<std::string> saves = {"w", "write", "wq", "x", "xit", "exit", "wa", "wall", "wqa", "wqall", "xa", "xall", "up", "update"};
    static const std::set<std::string> quits = {"q", "quit", "close", "clo", "wq", "x", "xit", "exit", "qa", "qall", "quitall", "wqa", "wqall", "xa", "xall"};
    static const std::set<std::string> alls = {"wa", "wall", "qa", "qall", "quitall", "wqa", "wqall", "xa", "xall"};
    bool save = saves.count(name) > 0, quit = quits.count(name) > 0, all = alls.count(name) > 0;
    // :e file and :sp file open it; :e alone reloads from disk.
    if (name == "e" || name == "edit" || name == "tabe" || name == "tabedit" || name == "sp" || name == "vs") {
        std::string argument = command.substr(std::min(command.size(), letters + (force ? 1 : 0)));
        while (!argument.empty() && argument.front() == ' ') {
            argument.erase(argument.begin());
        }
        if (argument.empty()) {
            if (Tab* tab = currentTab(); tab && tab->kind == Tab::Kind::Text && !tab->path().empty()) {
                if (tab->document->isDirty() && !force) {
                    return "E37: No write since last change (add ! to override)";
                }
                std::string error;
                tab->document->reloadFromDisk(&error);
            }
            return std::string();
        }
        runOnMain([this, argument] { openPath(argument); });
        return std::string();
    }
    if (name == "noh" || name == "nohlsearch") {
        findBar_->close();
        return std::string();
    }
    if (name == "bn" || name == "bnext" || name == "tabn" || name == "tabnext") {
        runOnMain([this] { runCommand("workbench.action.nextEditor"); });
        return std::string();
    }
    if (name == "bp" || name == "bprevious" || name == "tabp" || name == "tabprevious") {
        runOnMain([this] { runCommand("workbench.action.previousEditor"); });
        return std::string();
    }
    if (name == "term" || name == "terminal") {
        runOnMain([this] { runCommand("workbench.action.terminal.new"); });
        return std::string();
    }
    if (!save && !quit) {
        return "E492: Not an editor command: " + command;
    }
    if (save) {
        if (all) {
            saveAll();
        } else if (Tab* tab = currentTab()) {
            saveTab(*tab);
        }
    }
    if (!quit) {
        return std::string();
    }
    std::weak_ptr<bool> alive = alive_;
    if (all || !currentTab()) {
        runOnMain([this, alive] {
            if (!alive.expired()) {
                runCommand("workbench.action.closeWindow");
            }
        });
        return std::string();
    }
    Tab* tab = currentTab();
    if (!save && tab->dirty() && !force) {
        return "E37: No write since last change (add ! to override)";
    }
    uint64_t identifier = tab->identifier;
    // Closing takes the document away while the editor is still handling this key: do it afterwards,
    // once a save (which may format first) has finished.
    auto attempt = std::make_shared<std::function<void(int)>>();
    *attempt = [this, alive, identifier, force, attempt](int left) {
        if (alive.expired()) {
            return;
        }
        for (size_t index = 0; index < tabs_.size(); ++index) {
            if (tabs_[index]->identifier != identifier) {
                continue;
            }
            if (!tabs_[index]->dirty() || force) {
                removeTab(static_cast<long>(index));
            } else if (left > 0) {
                afterDelay(0.2, [attempt, left] { (*attempt)(left - 1); });
            }
            return;
        }
    };
    afterDelay(0, [attempt] { (*attempt)(15); });
    return std::string();
}

void MainWindow::vimAction(VimAction action) {
    auto dropSelection = [this] {
        Tab* tab = currentTab();
        if (!tab || tab->document->selection.empty()) {
            return;
        }
        Position start = tab->document->selection.range().start;
        editor_.selectRange({start, start});
    };
    switch (action) {
        case VimAction::Search:
        case VimAction::SearchBackward:
            runCommand("actions.find");
            break;
        case VimAction::SearchNext:
            findBar_->findNext();
            dropSelection();
            break;
        case VimAction::SearchPrevious:
            findBar_->findPrevious();
            dropSelection();
            break;
    }
}

// ---------------------------------------------------------------------------
// Extensions: the editor's state, and what the host asks for

void MainWindow::scheduleEditorStateForExtensions() {
    if (editorStateTimer_) {
        return;
    }
    std::weak_ptr<bool> alive = alive_;
    editorStateTimer_ = afterDelay(0.1, [this, alive] {
        if (alive.expired()) {
            return;
        }
        editorStateTimer_ = 0;
        sendEditorStateToExtensions();
    });
}

void MainWindow::sendEditorStateToExtensions() {
    Tab* tab = currentTab();
    if (!tab || tab->kind != Tab::Kind::Text || tab->path().empty()) {
        extensionHost_->setEditorState(Json());
        return;
    }
    const Document& document = *tab->document;
    const TextBuffer& buffer = document.buffer();
    auto selectionJson = [&](const Selection& selection) {
        return Json{{"anchor", lsp::position(buffer, selection.anchor)}, {"active", lsp::position(buffer, selection.caret)}};
    };
    Json selections = Json::array();
    selections.push_back(selectionJson(document.selection));
    for (const Selection& extra : document.extraSelections) {
        selections.push_back(selectionJson(extra));
    }
    extensionHost_->setEditorState({{"uri", lsp::fileURI(tab->path())},
                                    {"selections", selections},
                                    {"tabSize", Services::shared().settings().tabSize},
                                    {"insertSpaces", true}});
}

bool MainWindow::hostApplyWorkspaceEdit(const Json& edit) {
    return applyWorkspaceEdit(edit);
}

void MainWindow::hostOpenFile(const std::string& path, long line, long character) {
    if (line >= 0) {
        openPath(path, line + 1, character + 1);
    } else {
        openPath(path);
    }
}

bool MainWindow::hostSaveFile(const std::string& path) {
    if (Tab* tab = tabForPath(path)) {
        return saveTab(*tab);
    }
    return false;
}

void MainWindow::hostSaveAll() {
    saveAll();
}

bool MainWindow::hostRunBuiltinCommand(const std::string& identifier, const Json& arguments) {
    if (identifier == "vscode.open" && arguments.is_array() && !arguments.empty()) {
        const Json& target = arguments[0];
        std::string text;
        if (target.is_string()) {
            text = target.get<std::string>();
        } else if (target.is_object()) {
            text = target.value("external", target.value("path", ""));
        }
        if (text.empty()) {
            return false;
        }
        if (text.rfind("file:", 0) == 0) {
            openPath(lsp::pathFromURI(text));
        } else if (text.find("://") != std::string::npos || text.rfind("mailto:", 0) == 0) {
            openExternally(text);
        } else {
            openPath(text);
        }
        return true;
    }
    if (identifier == "workbench.action.reloadWindow" || identifier == "workbench.action.restartExtensionHost") {
        extensionHost_->restart();
        hostProvidersChanged();
        return true;
    }
    if (commands_.count(identifier)) {
        runCommand(identifier, arguments);
        return true;
    }
    return false;
}

void MainWindow::hostSetSelections(const Json& selections, const std::string& path) {
    Tab* tab = tabForPath(path);
    if (!tab || tab != currentTab() || !selections.is_array() || selections.empty()) {
        return;
    }
    const TextBuffer& buffer = tab->document->buffer();
    std::vector<Selection> parsed;
    for (const Json& item : selections) {
        try {
            parsed.push_back({lsp::position(buffer, item.at("anchor")), lsp::position(buffer, item.at("active"))});
        } catch (const std::exception&) {
        }
    }
    if (parsed.empty()) {
        return;
    }
    tab->document->selection = parsed.front();
    tab->document->extraSelections.assign(parsed.begin() + 1, parsed.end());
    editor_.selectionDidChange();
    editor_.jumpToPosition(parsed.front().caret);
}

void MainWindow::hostSetDecorations(const std::string& path, const std::string& key, const Json& options, const Json& ranges) {
    Tab* tab = tabForPath(path);
    if (!tab || tab->kind != Tab::Kind::Text) {
        return;
    }
    bool dark = Services::shared().theme().isDark();
    auto colorFrom = [](const Json& value) -> std::optional<Color> {
        if (value.is_string()) {
            return parseCssColor(value.get<std::string>());
        }
        if (value.is_object() && value.contains("id") && value["id"].is_string()) {
            std::string id = value["id"].get<std::string>();
            if (Services::shared().theme().hasColor(id)) {
                return Services::shared().color(id, 0);
            }
        }
        return std::nullopt;
    };
    auto merged = [dark](const Json& base, const Json& over) {
        Json result = base.is_object() ? base : Json::object();
        auto layer = [&](const Json& top) {
            if (!top.is_object()) {
                return;
            }
            for (const auto& [name, value] : top.items()) {
                if (name == "light" || name == "dark") {
                    continue;
                }
                if (name == "after" && value.is_object() && result.contains("after") && result["after"].is_object()) {
                    for (const auto& [inner, innerValue] : value.items()) {
                        result["after"][inner] = innerValue;
                    }
                } else {
                    result[name] = value;
                }
            }
            const char* variant = dark ? "dark" : "light";
            if (top.contains(variant) && top[variant].is_object()) {
                for (const auto& [name, value] : top[variant].items()) {
                    result[name] = value;
                }
            }
        };
        layer(base);
        layer(over);
        return result;
    };
    std::vector<Decoration> parsed;
    const TextBuffer& buffer = tab->document->buffer();
    if (ranges.is_array()) {
        for (const Json& item : ranges) {
            if (!item.is_object() || !item.contains("range")) {
                continue;
            }
            Json look = merged(options, item.value("renderOptions", Json::object()));
            Decoration decoration;
            try {
                decoration.range = {lsp::position(buffer, item["range"].at("start")), lsp::position(buffer, item["range"].at("end"))};
            } catch (const std::exception&) {
                continue;
            }
            if (look.contains("backgroundColor")) {
                decoration.background = colorFrom(look["backgroundColor"]);
            }
            if (look.contains("color")) {
                decoration.foreground = colorFrom(look["color"]);
            }
            decoration.wholeLine = look.value("isWholeLine", false);
            for (const char* name : {"borderColor", "outlineColor"}) {
                if (look.contains(name)) {
                    decoration.border = colorFrom(look[name]);
                }
            }
            for (const char* name : {"border", "outline"}) {
                if (!decoration.border && look.contains(name) && look[name].is_string()) {
                    std::string text = look[name].get<std::string>();
                    size_t space = text.rfind(' ');
                    decoration.border = parseCssColor(space == std::string::npos ? text : text.substr(space + 1));
                }
            }
            std::string weight = stringOr(look, "fontWeight");
            decoration.bold = weight == "bold" || weight == "bolder" || (weight.size() == 3 && weight[0] >= '6' && weight[0] <= '9');
            decoration.italic = look.value("fontStyle", "") == "italic";
            std::string textDecoration = look.value("textDecoration", "");
            decoration.underline = textDecoration.find("underline") != std::string::npos;
            decoration.strikethrough = textDecoration.find("line-through") != std::string::npos;
            if (look.contains("after") && look["after"].is_object()) {
                const Json& after = look["after"];
                decoration.afterText = after.value("contentText", "");
                if (after.contains("color")) {
                    decoration.afterColor = colorFrom(after["color"]);
                }
            }
            parsed.push_back(std::move(decoration));
        }
    }
    if (parsed.empty()) {
        tab->decorations.erase(key);
    } else {
        tab->decorations[key] = std::move(parsed);
    }
    if (tab == currentTab()) {
        editor_.setDecorationGroups(tab->decorations);
    }
    dirty_ = true;
}

void MainWindow::hostRevealRange(const Json& range, const std::string& path) {
    Tab* tab = tabForPath(path);
    if (!tab || tab != currentTab() || !range.is_object() || !range.contains("start")) {
        return;
    }
    try {
        editor_.jumpToPosition(lsp::position(tab->document->buffer(), range.at("start")));
    } catch (const std::exception&) {
    }
}

bool MainWindow::hostInsertSnippet(const std::string& snippet, const Json& ranges, const std::string& path) {
    Tab* tab = tabForPath(path);
    if (!tab || tab != currentTab() || tab->document->readOnly) {
        return false;
    }
    Json range = ranges.is_array() && !ranges.empty() ? ranges[0] : Json();
    if (range.is_null()) {
        const TextBuffer& buffer = tab->document->buffer();
        Range selected = tab->document->selection.range();
        range = {{"start", lsp::position(buffer, selected.start)}, {"end", lsp::position(buffer, selected.end)}};
    }
    auto item = std::make_shared<Suggestion>();
    item->resolved = true;
    item->kind = 15;
    item->label = "snippet";
    item->raw = std::make_shared<Json>(Json{{"label", "snippet"}, {"textEdit", {{"range", range}, {"newText", snippet}}}, {"insertTextFormat", 2}});
    insertSuggestion(*item);
    return true;
}

void MainWindow::hostProvidersChanged() {
    for (auto& tab : tabs_) {
        if (tab->kind == Tab::Kind::Text && !tab->path().empty()) {
            language_->trackDocument(tab->document.get());
        }
    }
    updateStatus();
}

void MainWindow::hostSetContextKey(const std::string& key, const std::string& value) {
    extensionContext_[key] = value;
}

void MainWindow::hostShowStatusMessage(const std::string& text, double seconds) {
    extensionMessage_ = text;
    updateStatus();
    if (seconds > 0 && !text.empty()) {
        std::weak_ptr<bool> alive = alive_;
        afterDelay(seconds, [this, alive, text] {
            if (!alive.expired() && extensionMessage_ == text) {
                extensionMessage_.clear();
                updateStatus();
            }
        });
    }
}

void MainWindow::hostStatusItemsChanged() {
    updateStatus();
}

void MainWindow::hostShowDialog(DialogOverlay::Options options, std::function<void(int button, const std::string& text)> done) {
    pushOverlay(std::make_unique<DialogOverlay>(std::move(options), std::move(done)));
}

void MainWindow::hostShowPalette(const std::string& placeholder, PaletteOverlay::Provider provider, std::function<void()> onDismiss) {
    pushOverlay(std::make_unique<PaletteOverlay>(placeholder, std::move(provider), std::move(onDismiss)));
}

}  // namespace gw::app
