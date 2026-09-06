/*
* Copyright (C) 2026 Andrew D. Harris
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <https://gnu.org>.
* Andrew D. Harris <andrew2325@gmail.com> used Gemini to make this app.  
* I am not an engineer or anything.
*/


#include <tcl.h>
#include <tk.h>
#include <cstdlib>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <curl/curl.h>

std::mutex g_mutex;
std::atomic<bool> g_is_done(false);
std::string g_response_string = "";

size_t WriteCallback(void *contents, size_t size, size_t nmemb, std::string *userp) {
    size_t totalSize = size * nmemb;
    userp->append((char*)contents, totalSize);
    return totalSize;
}

std::string EscapeJsonString(const std::string &input) {
    std::string output = "";
    for (char c : input) {
        switch (c) {
            case '\"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if ('\x00' <= c && c <= '\x1f') {
                    char buf[7];
                    snprintf(buf, sizeof(buf), "\\u00%02x", c);
                    output += buf;
                } else {
                    output += c;
                }
        }
    }
    return output;
}

std::vector<unsigned char> Base64Decode(const std::string &in) {
    std::string keys = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<unsigned char> out;
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; i++) T[keys[i]] = i;

    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (T[c] == -1) break;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back(char((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

std::string ExtractGeminiText(const std::string &json) {
    size_t textPos = json.find("\"text\": \"");
    if (textPos == std::string::npos) {
        textPos = json.find("\"text\":\"");
        if (textPos == std::string::npos) return json;
        textPos += 8;
    } else {
        textPos += 9;
    }

    std::string result = "";
    bool escaped = false;
    for (size_t i = textPos; i < json.length(); ++i) {
        char c = json[i];
        if (escaped) {
            if (c == 'n') result += '\n';
            else if (c == 't') result += '\t';
            else if (c == '"') result += '"';
            else if (c == '\\') result += '\\';
            else { result += '\\'; result += c; }
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            break;
        } else {
            result += c;
        }
    }
    return result.empty() ? json : result;
}

int SaveOutputCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[]) {
    Tcl_Eval(interp, "set output_content [.f_out.txt get 1.0 end-1c]");
    std::string content = Tcl_GetString(Tcl_GetObjResult(interp));

    Tcl_Eval(interp, "tk_getSaveFile -title \"Save Output As\" -filetypes {{\"Text Files\" {*.txt}} {\"All Files\" {*.*}}}");
    std::string filePath = Tcl_GetString(Tcl_GetObjResult(interp));

    if (filePath.empty()) return TCL_OK;

    std::ofstream outFile(filePath);
    if (outFile.is_open()) {
        outFile << content;
        outFile.close();
        Tcl_Eval(interp, "tk_messageBox -icon info -title \"Success\" -message \"File saved successfully!\"");
    } else {
        Tcl_Eval(interp, "tk_messageBox -icon error -title \"Error\" -message \"Could not open file for writing.\"");
    }
    return TCL_OK;
}

void BackgroundApiCall(std::string apiKey, std::string modelName, std::string prompt, std::string taskType) {
    int maxRetries = 3;
    int attempt = 0;
    bool success = false;
    std::string responseString = "";
    std::string escapedPrompt = EscapeJsonString(prompt);

    while (attempt < maxRetries && !success) {
        attempt++;
        responseString = "";
        CURL *curl = curl_easy_init();

        if (curl) {
            std::string url = "https://generativelanguage.googleapis.com/v1beta/models/" + modelName + ":generateContent?key=" + apiKey;
            std::string payload = "";

            if (taskType == "image") {
                payload = "{\"contents\":[{\"parts\":[{\"text\":\"" + escapedPrompt + "\"}]}],\"generationConfig\":{\"responseModalities\":[\"TEXT\",\"IMAGE\"]}}";
            } else if (taskType == "video") {
                payload = "{\"contents\":[{\"parts\":[{\"text\":\"" + escapedPrompt + "\"}]}],\"generationConfig\":{\"responseModalities\":[\"TEXT\",\"VIDEO\"]}}";
            } else {
                payload = "{\"contents\":[{\"parts\":[{\"text\":\"" + escapedPrompt + "\"}]}]}";
            }

            struct curl_slist *headers = NULL;
            headers = curl_slist_append(headers, "Content-Type: application/json");

            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseString);

            CURLcode res = curl_easy_perform(curl);
            curl_easy_cleanup(curl);
            curl_slist_free_all(headers);

            if (res != CURLE_OK) {
                responseString = "CURL Error: " + std::string(curl_easy_strerror(res));
                std::this_thread::sleep_for(std::chrono::seconds(2));
            } else {
                if (responseString.find("\"code\": 503") != std::string::npos || 
                    responseString.find("\"status\": \"UNAVAILABLE\"") != std::string::npos) {
                    if (attempt < maxRetries) {
                        std::this_thread::sleep_for(std::chrono::seconds(3));
                        continue;
                    }
                }
                success = true;
            }
        } else {
            responseString = "Failed to initialize cURL library.";
            break;
        }
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    g_response_string = responseString;
    g_is_done = true;
}

int CheckCompletionCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[]) {
    if (g_is_done) {
        std::string resp;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            resp = g_response_string;
        }

        size_t b64Pos = resp.find("inlineData\":{\"mimeType\":\"");
        if (b64Pos == std::string::npos) b64Pos = resp.find("\"data\":\"");

        if (b64Pos != std::string::npos) {
            // Check whether it's an image or video based on mimeType in response
            bool isVideo = (resp.find("video/") != std::string::npos);
            std::string ext = isVideo ? ".mp4" : ".png";
            std::string fileTypeName = isVideo ? "Videos" : "Images";
            std::string fileExtension = isVideo ? "*.mp4" : "*.png *.jpg";

            size_t dataStart = resp.find("\"data\":\"", b64Pos);
            if (dataStart != std::string::npos) {
                dataStart += 8;
                size_t dataEnd = resp.find("\"", dataStart);
                if (dataEnd != std::string::npos) {
                    std::string b64Data = resp.substr(dataStart, dataEnd - dataStart);
                    std::vector<unsigned char> decodedBytes = Base64Decode(b64Data);

                    std::string titleStr = isVideo ? "Save Generated Video" : "Save Generated Image";
                    std::string fileTypesOption = "{{\"" + fileTypeName + "\" {" + fileExtension + "}}}";
                    std::string tclCommand = "tk_getSaveFile -title \"" + titleStr + "\" -defaultextension " + ext + " -filetypes " + fileTypesOption;
                    
                    Tcl_Eval(interp, tclCommand.c_str());
                    std::string savePath = Tcl_GetString(Tcl_GetObjResult(interp));

                    if (!savePath.empty()) {
                        std::ofstream binFile(savePath, std::ios::binary);
                        if (binFile.is_open()) {
                            binFile.write((char*)decodedBytes.data(), decodedBytes.size());
                            binFile.close();
                            Tcl_Eval(interp, ".f_out.txt delete 1.0 end");
                            std::string msg = ".f_out.txt insert end \"[Media successfully saved to:\n" + savePath + "]\"";
                            Tcl_Eval(interp, msg.c_str());
                        } else {
                            Tcl_Eval(interp, ".f_out.txt insert end \"Error: Failed to write file to disk.\"");
                        }
                    } else {
                        Tcl_Eval(interp, ".f_out.txt delete 1.0 end");
                        Tcl_Eval(interp, ".f_out.txt insert end \"[Save operation cancelled by user]\"");
                    }
                } else {
                    Tcl_Eval(interp, ".f_out.txt delete 1.0 end");
                    Tcl_SetVar(interp, "rawResp", resp.c_str(), 0);
                    Tcl_Eval(interp, ".f_out.txt insert end $rawResp");
                }
            } else {
                Tcl_Eval(interp, ".f_out.txt delete 1.0 end");
                Tcl_SetVar(interp, "rawResp", resp.c_str(), 0);
                Tcl_Eval(interp, ".f_out.txt insert end $rawResp");
            }
        } else {
            std::string cleanText = ExtractGeminiText(resp);
            Tcl_SetVar(interp, "apiOutput", cleanText.c_str(), 0);
            Tcl_Eval(interp, ".f_out.txt delete 1.0 end");
            Tcl_Eval(interp, ".f_out.txt insert end $apiOutput");
        }
        Tcl_Eval(interp, ".f_btn.send configure -state normal");
    } else {
        Tcl_Eval(interp, "after 100 CheckCompletion");
    }
    return TCL_OK;
}

int SendPromptCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[]) {
    Tcl_Eval(interp, "set prompt_text [.f_prompt.txt get 1.0 end-1c]");
    std::string prompt = Tcl_GetString(Tcl_GetObjResult(interp));
    while (!prompt.empty() && (prompt.back() == '\n' || prompt.back() == '\r')) {
        prompt.pop_back();
    }

    Tcl_Eval(interp, "set api_key [.f_key.ent get]");
    std::string apiKey = Tcl_GetString(Tcl_GetObjResult(interp));

    Tcl_Eval(interp, ".f_model.cb get");
    std::string modelName = Tcl_GetString(Tcl_GetObjResult(interp));

    std::string taskType = "text";
    if (modelName.find("image") != std::string::npos) {
        taskType = "image";
    } else if (modelName.find("veo") != std::string::npos || modelName.find("video") != std::string::npos) {
        taskType = "video";
    }

    if (apiKey.empty()) {
        Tcl_Eval(interp, "tk_messageBox -icon warning -title \"Missing API Key\" -message \"Please enter your Gemini API key.\"");
        return TCL_OK;
    }

    if (prompt.empty()) {
        Tcl_Eval(interp, "tk_messageBox -icon warning -title \"Empty Prompt\" -message \"Please enter a prompt for Gemini.\"");
        return TCL_OK;
    }

    Tcl_Eval(interp, ".f_out.txt delete 1.0 end");
    std::string conn_msg = ".f_out.txt insert end \"Processing " + taskType + " request (" + modelName + ")...\n\"";
    Tcl_Eval(interp, conn_msg.c_str());
    Tcl_Eval(interp, ".f_btn.send configure -state disabled");

    g_is_done = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_response_string = "";
    }

    std::thread worker(BackgroundApiCall, apiKey, modelName, prompt, taskType);
    worker.detach();

    Tcl_Eval(interp, "after 100 CheckCompletion");

    return TCL_OK;
}

int main(int argc, char **argv) {
    Tcl_FindExecutable(argv[0]);
    Tcl_Interp *interp = Tcl_CreateInterp();

    if (Tcl_Init(interp) == TCL_ERROR) return 1;
    if (Tk_Init(interp) == TCL_ERROR) return 1;

    Tcl_CreateObjCommand(interp, "SendPromptCmd", SendPromptCmd, (ClientData)NULL, (Tcl_CmdDeleteProc *)NULL);
    Tcl_CreateObjCommand(interp, "SaveOutputCmd", SaveOutputCmd, (ClientData)NULL, (Tcl_CmdDeleteProc *)NULL);
    Tcl_CreateObjCommand(interp, "CheckCompletion", CheckCompletionCmd, (ClientData)NULL, (Tcl_CmdDeleteProc *)NULL);

    std::string ui_script = 
        "package require Tk\n"
        "wm title . \"Gemini Tk Client\"\n"
        "wm geometry . \"600x620\"\n"
        "wm resizable . 1 1\n"
        "option add *background #1e1e1e\n"
        "option add *foreground #ffffff\n"
        "option add *Label*background #1e1e1e\n"
        "option add *Label*foreground #ffffff\n"
        "option add *Text*background #2d2d2d\n"
        "option add *Text*foreground #ffffff\n"
        "option add *Entry*background #2d2d2d\n"
        "option add *Entry*foreground #ffffff\n"
        ". configure -background #1e1e1e\n"
        
        "label .title -text \"Gemini C++/Tk Client\" -font {Helvetica 12 bold}\n"
        "pack .title -pady 10\n"

        "frame .f_key\n"
        "pack .f_key -fill x -padx 15 -pady 5\n"
        "label .f_key.lbl -text \"API Key:\" -width 10 -anchor w\n"
        "entry .f_key.ent -show \"*\" -font {Helvetica 9}\n"
        "pack .f_key.lbl -side left\n"
        "pack .f_key.ent -side left -fill x -expand 1\n"

        "frame .f_model\n"
        "pack .f_model -fill x -padx 15 -pady 5\n"
        "label .f_model.lbl -text \"Model:\" -width 10 -anchor w\n"
        "ttk::combobox .f_model.cb -values {gemini-3.6-flash gemini-3.1-pro-preview gemini-3.1-flash-image veo-3.1-generate-preview} -state readonly\n"
        ".f_model.cb set \"gemini-3.6-flash\"\n"
        "pack .f_model.lbl -side left\n"
        "pack .f_model.cb -side left -fill x -expand 1\n"

        "frame .f_prompt\n"
        "pack .f_prompt -fill x -padx 15 -pady 5\n"
        "label .f_prompt.lbl -text \"Prompt:\" -width 10 -anchor nw\n"
        "text .f_prompt.txt -height 3 -font {Helvetica 9}\n"
        "pack .f_prompt.lbl -side left -anchor n\n"
        "pack .f_prompt.txt -side left -fill x -expand 1\n"

        "frame .f_btn\n"
        "pack .f_btn -fill x -padx 15 -pady 5\n"
        "button .f_btn.send -text \"Send Request\" -command SendPromptCmd -bg #007acc -fg white -relief flat -font {Helvetica 9 bold} -padx 10 -pady 4\n"
        "button .f_btn.save -text \"Save Output As...\" -command SaveOutputCmd -bg #28a745 -fg white -relief flat -font {Helvetica 9 bold} -padx 10 -pady 4\n"
        "pack .f_btn.send -side right -padx 5\n"
        "pack .f_btn.save -side right -padx 5\n"

        "frame .f_out\n"
        "pack .f_out -fill both -expand 1 -padx 15 -pady 10\n"
        "text .f_out.txt -relief flat -font {Courier 9} -wrap word -yscrollcommand {.f_out.scr set}\n"
        "scrollbar .f_out.scr -command {.f_out.txt yview}\n"
        "pack .f_out.txt -side left -fill both -expand 1\n"
        "pack .f_out.scr -side right -fill y\n";

    if (Tcl_Eval(interp, ui_script.c_str()) == TCL_ERROR) {
        const char *err = Tcl_GetStringResult(interp);
        fprintf(stderr, "Tcl Error: %s\n", err);
        return 1;
    }

    Tk_MainLoop();
    return 0;
}
