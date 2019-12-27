#define _WIN32_WINNT 0x501
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <string>
#include <iostream>
#include <limits>
#include <ctime>
#include <utility>
#include <queue>
#include <vector>

// running into compilation error about WinMain not having defined reference
// need to define SDL_MAIN_HANDLED
// this is interesting? https://djrollins.com/2016/10/02/sdl-on-windows/
//#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>

#define DEFAULT_PORT 2000
#define DEFAULT_BUFLEN 60000
#define MSG_HEADER 6 // this is the header that comes with image data chunk packets 

#define WINDOW_WIDTH 600
#define WINDOW_HEIGHT 400

struct ThreadParams {
	std::string ipAddr;
	SDL_Renderer* renderer;
	bool* endThread;
};

void logSDLError(std::ostream &os, const std::string &msg){
	os << msg << " error: " << SDL_GetError() << std::endl;
}


void talkToServer(ThreadParams params){
	SDL_Renderer* renderer = params.renderer;
	
	// functor for comparing for priority_queue 
	struct Compare{
		bool operator()(const std::pair<int, uint8_t*> a, const std::pair<int, uint8_t*> b){
			return a.first > b.first; // why isn't it a.first < b.first?
									  // I think it's because priority_queue by default sorts by descending order.
		}
	};
	
	// buffer to hold all pixel data 
	// use a priority queue (or min heap) to keep order of the packets since they could come in 
	// out of order. 
	// use a pair to store the data like this: (packetNum, pixelDataArray) in the queue. sort by packetNum.
	// https://stackoverflow.com/questions/37318537/deciding-priority-in-case-of-pairint-int-inside-priority-queue
	// https://stackoverflow.com/questions/12508496/comparators-in-stl
	// https://stackoverflow.com/questions/356950/what-are-c-functors-and-their-uses
	std::priority_queue<std::pair<int, uint8_t*>, 
						std::vector<std::pair<int, uint8_t*>>,
						Compare> pqueue;
	
	
	// variables for networking
	WSADATA wsaData;
	int iResult;
	char recvbuf[DEFAULT_BUFLEN + MSG_HEADER];
	int recvbuflen = DEFAULT_BUFLEN + MSG_HEADER;
	int rtnVal;
	
	iResult = WSAStartup(MAKEWORD(2,2), &wsaData);
	if(iResult != 0){
		printf("WSAStartup failed: %d\n", iResult);
		exit(1);
	}
	
	struct sockaddr_in servAddr;
	ZeroMemory(&servAddr, sizeof(servAddr));
	servAddr.sin_family = AF_INET;

	const char* theIp = params.ipAddr.c_str();// = "127.0.0.1"; // pass in ip as arg to client?
	
	int size = sizeof(servAddr);
	iResult = WSAStringToAddressA((LPSTR)theIp, AF_INET, NULL, (struct sockaddr *)&servAddr, &size);
	if(iResult != 0){
		printf("getaddrinfo failed: %d\n", iResult);
		WSACleanup();
	}else{
		servAddr.sin_port = htons(DEFAULT_PORT);
	}
	
	SOCKET connectSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if(connectSocket == INVALID_SOCKET){
		printf("socket() failed\n");
		WSACleanup();
		exit(1);
	}

	// maybe move all this stuff to a separate thread?
	// then in this main thread we can handle some basic SDL2 window stuff, like closing out the window
	printf("preparing to message server...\n");
	std::string str = "hellooo there"; 
	const char* msg = str.c_str();
	
	bool sentInitMsg = false;
	
	int packetsToExpect = 0;
	int packetsReceived = 0;
	int regularPacketDataSize = 60000; // this is specific to the current parameters (image coming in is 960000 bytes)
	//int lastPacketDataSize = 0;
	int lastFrameId = -1;
	
	// how do we know when we should process all the chunks of a frame? we should expect packet loss,
	// so we don't really know when we've seen the last packet of a frame?
	// since on the server side we have a sleep in between frames, is it very unlikely that a packet for the 
	// next frame could slip in with the packets of the current frame? so we can look at frame id maybe?
	
	// we can cheat a little for now. we know exactly how big the image is coming in,
	// so we know exactly how many pixel data bytes can go into each packet. 
	// for now, let's use this knowledge (i.e. there should be 16 packets, each one with 
	// 60000 bytes of data except the last packet, which has 50000). this is beacause we hardcoded
	// the width and height of the image, which is 600 and 400, respectively.
	int dataSize = regularPacketDataSize;
	SDL_Texture* texBuf = nullptr;
	
	while(1){
		
		// clear the recv buffer 
		ZeroMemory(recvbuf, recvbuflen);
		
		// send msg 
		if(!sentInitMsg){
			int bytesSent = sendto(connectSocket, msg, str.length(), 0, (struct sockaddr *)&servAddr, size);
			if(bytesSent < 0){
				printf("sendto failed.\n");
				exit(1);
			}
			std::cout << "sent hello msg" << std::endl;
			sentInitMsg = true;
		}
		
		// receive msg
		rtnVal = recvfrom(connectSocket, recvbuf, recvbuflen, 0, (struct sockaddr *)&servAddr, &size);
		if(rtnVal > 0){	
			// check recvbuf 
			if(recvbuf[0] == 'i'){
				printf("Got data about the next image frame: %s\n", recvbuf);
				printf("num packets to expect for image: %d\n", (int)recvbuf[4]);
				std::cout << "--------------" << std::endl;
				
				packetsToExpect = (int)recvbuf[4];
			}else if(recvbuf[0] == 'p'){
				
				uint8_t* pixels = (uint8_t*)recvbuf;
				
				// packet chunk 
				int packetNum = (int)pixels[1];
				int currFrameId = (int)pixels[3];
				//std::cout << "frame id: " << (int)pixels[3] << std::endl;
				//std::cout << "data size for this packet: " << (int)pixels[5] << std::endl;
				//std::cout << "first byte of data: " << (int)pixels[6] << std::endl;
				
				if(lastFrameId == -1){
					lastFrameId = currFrameId;
				}
				
				if(currFrameId == lastFrameId){
					// add the current packet being looked at to the priority queue
					// need to make a copy of the received data 
					// note that this is hardcoded right now based on assumptions of the image data! (assumes 600 x 400 window)
					//int dataSize = regularPacketDataSize;
					uint8_t* pixelsCopy = new uint8_t[dataSize];
					memcpy(pixelsCopy, recvbuf+6, dataSize);
					
					std::pair<int, uint8_t*> packetChunk(packetNum, pixelsCopy);
					std::cout << "adding packet num: " << packetNum << " to queue" << " for frame id: " << currFrameId << std::endl;
					pqueue.push(packetChunk); 
					
					packetsReceived++;
				}
				
				// if we get most of the packets? (probably rethink this later)
				// just have an alarm set up if hanging more than 2 sec?
				if((float)packetsReceived == (packetsToExpect)){
					
					std::vector<uint8_t> pixelData;
					std::cout << "got at least half the packets..." << std::endl;
					int lastPacketNum = 0;
					
					while(!pqueue.empty()){
						
						// collect all the data for the image
						
						// neat! http://forums.codeguru.com/showthread.php?509825-std-vectors-Append-chunk-of-data-to-the-end
						// https://stackoverflow.com/questions/7593086/why-use-non-member-begin-and-end-functions-in-c11
						int currPacketNum = pqueue.top().first;
						//std::cout << "packet number: " << currPacketNum << std::endl;
						uint8_t* data = pqueue.top().second;
						
						if(currPacketNum != lastPacketNum + 1){
							for(int i = lastPacketNum+1; i < currPacketNum; i++){
								std::cout << "filling in data for missing packet num: " << i << std::endl;
								// fill with rgba(255,255,255,255) for any missing packets between last packet num and current 
								uint8_t* filler = new uint8_t[dataSize];
								memset(filler, 255, dataSize);
								pixelData.insert(pixelData.end(), filler, filler+dataSize);
								delete[] filler;
							}
						}
						
						// this is the current packet's data
						pixelData.insert(pixelData.end(), data, data+dataSize);

						delete[] data;
						lastPacketNum = currPacketNum;
						pqueue.pop();
						
						// this means we won't look at any more packets that might've come in after 
						// we processed this frame. this step forces the priority queue to only add
						// packets for the next frame, 
						lastFrameId = currFrameId+1 % 100;
					}
					
					// if missing last packet, fill in 
					std::cout << "lastPacketNum seen: " << lastPacketNum << std::endl;
					std::cout << "num packets expected: " << packetsToExpect << std::endl;
					for(int i = lastPacketNum+1; i <= packetsToExpect; i++){
						std::cout << "filling in data for missing packet num after last packet: " << i << std::endl;
						uint8_t* filler = new uint8_t[dataSize];
						memset(filler, 255, dataSize);
						pixelData.insert(pixelData.end(), filler, filler+dataSize);
						delete[] filler;
					}
					
					std::cout << "--------------" << std::endl;
					
					// form image
					// https://stackoverflow.com/questions/21007329/what-is-an-sdl-renderer
					// try this: https://gamedev.stackexchange.com/questions/102490/fastest-way-to-render-image-data-from-buffer
					// this might help too? https://www.gamedev.net/forums/topic/683956-blit-a-byte-array-of-pixels-to-screen-in-sdl-fast/
					std::cout << "the total size of the image data in bytes: " << pixelData.size() << std::endl;
					uint8_t* imageData = new uint8_t[(int)pixelData.size()];
					
					if(texBuf == nullptr){
						texBuf = SDL_CreateTexture(
									renderer,
									SDL_PIXELFORMAT_ARGB8888,
									SDL_TEXTUREACCESS_STREAMING,
									WINDOW_WIDTH,
									WINDOW_HEIGHT);
					}
					
					int pitch = WINDOW_WIDTH * 4; // 4 bytes per pixel
					SDL_LockTexture(texBuf, NULL, (void**)&imageData, &pitch);
					
					// fill the texture pixel buffer
					memcpy(imageData, (uint8_t *)&pixelData[0], (int)pixelData.size());
					
					// try to clear the previous frame so we don't carry over any drawn pixels 
					// https://gamedev.stackexchange.com/questions/131252/how-do-i-clear-streaming-textures-in-sdl
					SDL_UnlockTexture(texBuf);
					SDL_RenderCopy(renderer, texBuf, NULL, NULL);
					SDL_RenderPresent(renderer);
					
					delete[] imageData;
					
					// reset
					packetsReceived = 0;
					
					// get new frame from server
					int bytesSent = sendto(connectSocket, msg, str.length(), 0, (struct sockaddr *)&servAddr, size);
					if(bytesSent < 0){
						printf("sendto for new frame failed.\n");
						exit(1);
					}
				} // end process image 
				
			}else{
				// frame data. rebuild image from the chunks and display
				printf("Message received from server: %s\n", recvbuf);
			}
		}else if(rtnVal == 0){
			printf("connection closed.\n");
			exit(1);
		}else{
			printf("recv failed: %d\n", WSAGetLastError());
			exit(1);
		}	
	}
	SDL_DestroyTexture(texBuf);
}


// to be used in CreateThread
DWORD WINAPI threadAction(LPVOID lpParam){
	talkToServer(*(ThreadParams *)lpParam);
	return 0;
}


int main(int argc, char *argv[]){
	
	if(SDL_Init(SDL_INIT_VIDEO) != 0){
		logSDLError(std::cout, "SDL_Init Error: ");
		return 1;
	}
	
	printf("starting client...\n");
	
	// create a window 
	SDL_Window* window = SDL_CreateWindow("winsock-udp-screencast", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_SHOWN);
	if(window == nullptr){
		logSDLError(std::cout, "SDL_CreateWindow Error: ");
		SDL_Quit();
		return 1;
	}
	
	// create the renderer to render the window with 
	// pass this as an argument to the thread handling the communication with the server
	SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if(renderer == nullptr){
		SDL_DestroyWindow(window);
		logSDLError(std::cout,"SDL_CreateRenderer Error: ");
		SDL_Quit();
		return 1;
	}
	
	/***
		START EVENT LOOP 
	***/
	SDL_Event event;
	bool quit = false;
	bool spawnThread = false;
	
	ThreadParams params;
	params.renderer = renderer;
	
	std::string ip = "127.0.0.1";
	if(argc == 2){
		ip = std::string(argv[1]);
	}
	params.ipAddr = ip;
	
	//while(1){
	while(!quit){
		while(SDL_PollEvent(&event)){
			if(event.type == SDL_QUIT){
				quit = true;
			}
		}
		if(!spawnThread){
			// make sure this only occurs once!
			CreateThread(NULL, 0, threadAction, (void *)&params, 0, 0);	
			spawnThread = true;
		}
	}
	
	// cleanup
	WSACleanup();
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);
	SDL_Quit();
	return 0;
}
