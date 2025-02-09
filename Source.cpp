#include<WinSock2.h>
#include<WS2tcpip.h>
#include<shlwapi.h>
#include "imgui.h"
#include"imgui_stdlib.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <d3d11.h>
#include <tchar.h> 
#include<iostream>
#include<string>
#include<thread>
#include<vector>
#include<unordered_map>
#include<atomic>
#include<fstream>
#include<random>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib,"shlwapi.lib")
//---------imgui---------
// Data
static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static bool                     g_SwapChainOccluded = false;
static UINT                     g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

// Forward declarations of helper functions
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);


// --------network data------------
#define PORT "54000"
//#define IP "127.0.0.1"
#define IP "47.93.62.111"
#define MAX_BUFFER_SIZE 1024

static SOCKET connectSocket = INVALID_SOCKET;
static std::atomic<bool>network_running = true;

//-----chat data-------------------
//(1) public chat
static std::string mainChainInput;
static std::vector<std::string> mainChatHistory;
//(2) private chat and users information 
//Different computers generate different ID to distinguish users.  
static std::string myID;
static std::string myNickname;
static std::string tempNickname;
struct userInfo {
    std::string nickname;
    bool isOnline = true;
    bool private_isOpen = false;
    std::vector<std::string> private_chatHistory;
    std::string private_inputBuffer;
};
static std::unordered_map<std::string, userInfo>Users;

//-----functions for network-----
void receiveMessages();
void heartbeat();
std::string GenerateUUID();
void loadOrCreateUUID();
void saveUUID();


// Main code
int main()
{
    // (1) generate or load UUID
	loadOrCreateUUID();

    // (2)Initialize winsock
	WSADATA wsaData;
	int iResult;
	iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (iResult != 0)
	{
		std::cout << "WSAStartup failed with error: " << iResult << std::endl;
		return 1;
	}

	//(3) get addrinfo
    addrinfo* result = NULL, hints;
	ZeroMemory(&hints, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	iResult = getaddrinfo(IP,PORT, &hints, &result);
    if (iResult != 0)
    {
		std::cout << "getaddrinfo failed with error: " << iResult << std::endl;
		WSACleanup();
		return 1;
	}

	//(4) create socket
	connectSocket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (connectSocket == INVALID_SOCKET)
    {
        std::cout << "socket failed with error: " << WSAGetLastError() << std::endl;
        freeaddrinfo(result);
		WSACleanup();
		return 1;
    }

	//(5) Connect to server.
	iResult = connect(connectSocket, result->ai_addr, (int)result->ai_addrlen);
	if (iResult == SOCKET_ERROR)
	{
		closesocket(connectSocket);
		connectSocket = INVALID_SOCKET;
		std::cout << "Unable to connect to server!" << std::endl;
		return 1;
	}
	freeaddrinfo(result);

	//(6)send ID to server
    {
        std::string handshakeMsg = "ID:" + myID + "\n" + "NICK:" + myNickname + "\n";
		send(connectSocket, handshakeMsg.c_str(), handshakeMsg.size(), 0);
    }

	//(7)start threads to receive messages and send heartbeat
	std::thread reth(receiveMessages);
	reth.detach();
	std::thread hbth(heartbeat);
    hbth.detach();

	//(8) Initialize Direct3D and win32
    // Create application window
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"ImGui", nullptr };
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"Chat Room", WS_OVERLAPPEDWINDOW, 100, 100, 1280, 800, nullptr, nullptr, wc.hInstance, nullptr);

    // Initialize Direct3D
    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    // Show the window
    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    //(9)Initial imgui
    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    //ImGui::StyleColorsLight();
    ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
    float inputAreaHeight = 40.0f;
    ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);

    // Setup Platform/Renderer backends
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    //(10) Main loop
    bool done = false;
    while (!done)
    {
        // Poll and handle messages (inputs, window resize, etc.)
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (!network_running) done = true;
        if (done) break;

        // Handle window being minimized or screen locked
        if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)
        {
            ::Sleep(10);
            continue;
        }
        g_SwapChainOccluded = false;

        // Handle window resize (we don't resize directly in the WM_SIZE handler)
        if (g_ResizeWidth != 0 && g_ResizeHeight != 0)
        {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        // Start the Dear ImGui frame
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

		//-------main chat window-------
        ImGui::Begin("Chat Room"); 
        {
			//------local user information--------
			ImGui::Text("Your UUID : %s", myID.c_str());
			ImGui::Text("Your nickname:");
			ImGui::SameLine();
			ImGui::InputText("##nickname", &tempNickname);

			if (ImGui::Button("Update Nickname"))
			{
				std::string sendMsg = "NICK:" + tempNickname;
				send(connectSocket, sendMsg.c_str(), sendMsg.size(), 0);
				//save the nickname
                myNickname = tempNickname;
				saveUUID();
			}
			ImGui::SameLine();
            if (ImGui::Button("Reset")) {
				tempNickname = myNickname;
            }
			ImGui::Separator();
            
          
			//------users list on the left-------
			ImGui::BeginChild("Users", ImVec2(150, 0), true);
			ImGui::Text("Users List");
            ImGui::Separator();
            for (auto& user : Users)
			{
                ImGui::PushStyleColor(ImGuiCol_Text,
                    user.second.isOnline ?
                    ImVec4(0.4f, 1.0f, 0.4f, 1.0f) :  // online color
                    ImVec4(0.6f, 0.6f, 0.6f, 1.0f));  // offline color
				if (ImGui::Selectable(user.second.nickname.c_str()))
				{
					user.second.private_isOpen = true;
				}
                ImGui::PopStyleColor();

            }
			ImGui::EndChild();
			ImGui::SameLine();

			//------main chat on the right-------
			ImGui::BeginChild("public Chat",ImVec2(0,0),true, ImGuiWindowFlags_NoScrollbar);
            {
                float msgHeight = ImGui::GetWindowHeight() - inputAreaHeight-30;
                ImGui::BeginChild("Chat Messages", ImVec2(0, msgHeight), true, ImGuiWindowFlags_HorizontalScrollbar| ImGuiWindowFlags_AlwaysVerticalScrollbar);
                {
                    for (auto& msg : mainChatHistory) {
                        ImGui::TextWrapped(msg.c_str());
                    }
                    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 5) {
                        ImGui::SetScrollHereY(1.0f);
                    }
                }
			    ImGui::EndChild();
			    ImGui::Separator();
			    //-----------------input area-----------------
			    ImGui::BeginChild("Input Area", ImVec2(0, inputAreaHeight), false);
                {
                    //ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5);
                    ImGui::Text("Message:");
                    ImGui::SameLine();

                    ImGui::InputText("##mainChatInput", &mainChainInput);
                    ImGui::SameLine();
                    if (ImGui::Button("Send")) {
                        if (!mainChainInput.empty())
                        {
                            std::string publicMsg = "MSG:" + mainChainInput;
                            send(connectSocket, publicMsg.c_str(), publicMsg.size(), 0);
                            mainChatHistory.push_back(myNickname + ":" + mainChainInput);
                            mainChainInput.clear();
                        }
                    }
                }
			    ImGui::EndChild();
            }
            ImGui::EndChild();
        }
		ImGui::End();


		//-------private chat window-------
		for (auto& user : Users)
		{
			if (user.second.private_isOpen)
			{
				std::string title = "Private Chat with " + user.second.nickname;
                ImGui::SetNextWindowSizeConstraints(ImVec2(400, 300), ImVec2(FLT_MAX, FLT_MAX));
                //chat history
                ImGui::Begin(title.c_str(),&user.second.private_isOpen);
				ImGui::BeginChild("Private Chat Messages", ImVec2(0, 220), true, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_AlwaysVerticalScrollbar);
                {
                    for (auto& msg : user.second.private_chatHistory)
                    {
                        ImGui::TextWrapped(msg.c_str());
                    }
                    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 5) {
                        ImGui::SetScrollHereY(1.0f);
                    }
                }
				ImGui::EndChild();
				ImGui::Separator();
				
                //input box
				ImGui::Text("Private Message:");
				ImGui::SameLine();
				ImGui::PushItemWidth(300);
				ImGui::InputText("##privateChatInput", &user.second.private_inputBuffer);
				ImGui::PopItemWidth();
				ImGui::SameLine();
				if (ImGui::Button("Send"))
				{
					if (!user.second.private_inputBuffer.empty())
					{
                        std::string privateMsg = "PRIV:" + user.first + "|" + user.second.private_inputBuffer;
                        send(connectSocket, privateMsg.c_str(), privateMsg.size(), 0);
						user.second.private_chatHistory.push_back(myNickname+":"+ user.second.private_inputBuffer);
						user.second.private_inputBuffer.clear();
					}
				}
				ImGui::End();
			}
		}

		// Rendering
		ImGui::Render();
		const float clear_color_with_alpha[4] = { clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w };
		g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
		g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        // Present
        HRESULT hr = g_pSwapChain->Present(1, 0);   // Present with vsync
        //HRESULT hr = g_pSwapChain->Present(0, 0); // Present without vsync
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    // Cleanup
	network_running = false;
	closesocket(connectSocket);
	WSACleanup();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}

// Helper functions

bool CreateDeviceD3D(HWND hWnd)
{
    // Setup swap chain
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    //createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED) // Try high-performance WARP software driver if hardware is not available.
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK)
        return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget()
{
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget()
{
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);


LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
            return 0;
        g_ResizeWidth = (UINT)LOWORD(lParam); // Queue resize
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
            return 0;
        break;
    case WM_CLOSE:
        network_running = false;
		closesocket(connectSocket);
		DestroyWindow(hWnd);
		return 0;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

void receiveMessages()
{
    char recvbuf[MAX_BUFFER_SIZE];
    while (network_running)
    {
        int iResult = recv(connectSocket, recvbuf, MAX_BUFFER_SIZE, 0);
        if (iResult > 0)
        {
            recvbuf[iResult] = '\0';
            std::string msg(recvbuf);
			std::cout << "Received: " << msg << std::endl;
            // public chat message
            if (msg.rfind("MSG:", 0) == 0)
            {
                std::string chatText = msg.substr(4);
				// add to public chat history
                mainChatHistory.push_back(chatText);
            }
			// private chat message
            else if (msg.rfind("[Private]", 0) == 0)
            {
				// private chat message
                std::string content = msg.substr(9);
                size_t colonPos = content.find(':');
                if (colonPos != std::string::npos)
                {
                    std::string senderID = content.substr(0, colonPos);   
                    std::string privateMsg = content.substr(colonPos + 1);      
                    auto it = Users.find(senderID);
                    if (it != Users.end())
                    {
						// save the message to the user's private chat history
                        it->second.private_chatHistory.push_back(it->second.nickname +":"+privateMsg);
                    }
                    else
                    {
						// create a new user
                        userInfo newUser;
                        newUser.nickname = "Unknown(" + senderID + ")";
                        newUser.private_chatHistory.push_back(newUser.nickname + ":" + privateMsg);
                        Users.insert({ senderID, newUser });
                    }
                }
			}
			// user joined message
            else if (msg.rfind("USERJOIN:", 0) == 0)
            {
                std::string content = msg.substr(9);
                size_t colonPos = content.find(':');
                if (colonPos != std::string::npos)
                {
                    std::string uid = content.substr(0, colonPos);
                    std::string nick = content.substr(colonPos + 1);

                    userInfo& u = Users[uid];
                    u.nickname = nick;
                    u.isOnline = true;

                    mainChatHistory.push_back("[System] User joined: " + nick);
                }
            }
			//user left message
            else if (msg.rfind("USERLEFT:", 0) == 0)
            {
                std::string leavingID = msg.substr(9);
                auto it = Users.find(leavingID);
                if (it != Users.end())
                {
                    mainChatHistory.push_back("[System] User left: " + it->second.nickname);
                    it->second.isOnline = false;
                }
                else
                {
                    mainChatHistory.push_back("[System] Unknown user left: " + leavingID);
                }
            }
			// nickname change message
            else if (msg.rfind("NICKCHANGE:", 0) == 0)
            {
                std::string content = msg.substr(11);
                size_t colonPos = content.find(':');
                if (colonPos != std::string::npos)
                {
                    std::string uid = content.substr(0, colonPos);
                    std::string newNick = content.substr(colonPos + 1);

                    auto it = Users.find(uid);
                    if (it != Users.end())
                    {
                        std::string oldName = it->second.nickname;
                        it->second.nickname = newNick;
                        mainChatHistory.push_back("[System] " + oldName + " changed nickname to " + newNick);
                    }
                }
            }
			// user list message
            else if (msg.rfind("USERS:", 0) == 0)
            {
                std::string content = msg.substr(6);
                size_t startPos = 0;
                while (true)
                {
                    size_t endPos = content.find('\n', startPos);
                    if (endPos == std::string::npos) endPos = content.size();

                    std::string line = content.substr(startPos, endPos - startPos);
                    if (!line.empty()) {
                        size_t cpos = line.find(':');
                        if (cpos != std::string::npos) {
                            std::string uid = line.substr(0, cpos);
                            std::string nick = line.substr(cpos + 1);
                            
                            userInfo& u = Users[uid];
                            u.nickname = nick;
                            u.isOnline = true;
                        }
                    }
                    if (endPos == content.size()) break;
                    startPos = endPos + 1;
                }
            }

            else
            {
                std::cout << "Unknown msg: " << msg << std::endl;
            }
        }
        else if (iResult == 0)
        {
            std::cout << "Connection closed by server\n";
            network_running = false;
        }
        else
        {
            std::cout << "recv failed with error: " << WSAGetLastError() << std::endl;
            network_running = false;
        }
    }
}

void heartbeat()
{
    std::string pingMsg = "PING";
	while (network_running)
	{
		std::this_thread::sleep_for(std::chrono::seconds(30));
		send(connectSocket, pingMsg.c_str(), pingMsg.size(), 0);
	}
}

std::string GenerateUUID()
{
    static const char* hex = "0123456789ABCDEF";
	std::random_device rd;
	std::mt19937 gen(rd());
	std::uniform_int_distribution<> dis(0, 15);
    std::string uuid(32, '0');
	for (int i = 0; i < 32; i++)
	{
		uuid[i] = hex[dis(gen)];
	}
	return uuid;
}

void loadOrCreateUUID()
{
	char exePath[MAX_PATH];
	::GetModuleFileNameA(NULL, exePath, MAX_PATH);
	::PathRemoveFileSpecA(exePath);

	char idFilePath[MAX_PATH];
	::PathCombineA(idFilePath, exePath, "id.txt");
	{
        std::ifstream idFile(idFilePath);
		if (idFile.is_open())
		{
            std::string idLine, nicknameLine;
            if (std::getline(idFile, idLine)) {
                if (!idLine.empty()) {
					myID = idLine;
                }
            }
            if (std::getline(idFile, nicknameLine)) {
                if (!nicknameLine.empty())
                {
                    myNickname = nicknameLine;
                    tempNickname = myNickname;
                }
            }
			idFile.close();
		}
		else
		{
			myID = GenerateUUID();
			std::string last4 = myID.substr(myID.size() - 4);
			myNickname = "User" + last4;
            tempNickname = myNickname;

			std::ofstream idFile(idFilePath);
			if (idFile.is_open())
			{
				idFile << myID << "\n";
				idFile << myNickname << "\n";
				idFile.close();
			}
		}
	}
}

void saveUUID()
{
    char exePath[MAX_PATH];
    ::GetModuleFileNameA(NULL, exePath, MAX_PATH);
    ::PathRemoveFileSpecA(exePath);

    char idFilePath[MAX_PATH];
    ::PathCombineA(idFilePath, exePath, "id.txt");
    {
	    std::ofstream idFile(idFilePath);
	    if (idFile.is_open())
	    {
		    idFile << myID << "\n";
		    idFile << myNickname << "\n";
		    idFile.close();
	    }
    }
}