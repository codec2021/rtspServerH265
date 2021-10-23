// RTSP Server
#include "xop/RtspServer.h"
#include "net/Timer.h"
#include <thread>
#include <memory>
#include <iostream>
#include <string>

#define MAX_FRAME_SIZE 500000

class H265File
{
public:
	H265File(int buf_size = MAX_FRAME_SIZE);
	~H265File();

	bool Open(const char *path);
	void Close();

	bool IsOpened() const
	{ 
		return (m_file != NULL); 
	}

	int ReadFrame(char* in_buf, int in_buf_size, bool* end);
    
private:
	FILE *m_file		= NULL;
	char *m_buf			= NULL;
	int  m_buf_size		= 0;
	int  m_bytes_used	= 0;
	int  m_count		= 0;
};


void SendFrameThread(xop::RtspServer* rtsp_server, xop::MediaSessionId session_id, H265File* h265_file)
{       
	int buf_size = MAX_FRAME_SIZE;
	std::unique_ptr<uint8_t> frame_buf(new uint8_t[buf_size]);

	while(1) 
	{
		bool end_of_frame = false;
		int frame_size = h265_file->ReadFrame((char*)frame_buf.get(), buf_size, &end_of_frame);
		//fprintf(stderr,"send stream %d bits\n", frame_size << 3);

		if(frame_size > 0) 
		{
			xop::AVFrame videoFrame = {0};
			videoFrame.type = 0; 
			videoFrame.size = frame_size;
			videoFrame.timestamp = xop::H265Source::GetTimestamp();
			videoFrame.buffer.reset(new uint8_t[videoFrame.size]);    
			memcpy(videoFrame.buffer.get(), frame_buf.get(), videoFrame.size);
			rtsp_server->PushFrame(session_id, xop::channel_0, videoFrame);
		}
		else 
		{
			fprintf(stderr, "send stream size is %d\n", frame_size);
			break;
		}
		xop::Timer::Sleep(40); 
	};

	return;
}


H265File::H265File(int buf_size)
    : m_buf_size(buf_size)
{
	m_buf = new char[m_buf_size];
}

H265File::~H265File()
{
	delete m_buf;
}

bool H265File::Open(const char *path)
{
	m_file = fopen(path, "rb");
	if(m_file == NULL) 
	{      
		return false;
	}
	return true;
}

void H265File::Close()
{
	if(m_file) 
	{
		fclose(m_file);
		m_file = NULL;
		m_count = 0;
		m_bytes_used = 0;
	}
}

int H265File::ReadFrame(char* in_buf, int in_buf_size, bool* end)
{
	if(m_file == NULL) 
	{
		return -1;
	}

	int bytes_read = (int)fread(m_buf, 1, m_buf_size, m_file);
	if(bytes_read == 0) 
	{
		fseek(m_file, 0, SEEK_SET); 
		m_count			= 0;
		m_bytes_used	= 0;
		bytes_read = (int)fread(m_buf, 1, m_buf_size, m_file);
		if(bytes_read == 0)         
		{            
			this->Close();
			return -1;
		}
	}

	bool is_find_start = false;
	bool is_find_end   = false;
	int i = 0, start_code = 3;
	*end = false;

	int code = 0;
	for (i = 0; i < bytes_read - 5; i++) 
	{
		if(m_buf[i] == 0 && m_buf[i + 1] == 0 && m_buf[i + 2] == 1) 
		{
			start_code = 3;
		}
		else if(m_buf[i] == 0 && m_buf[i + 1] == 0 && m_buf[i + 2] == 0 && m_buf[i + 3] == 1) 
		{
			start_code = 4;
		}
		else  
		{
			continue;
		}
		
		code = m_buf[i + start_code];
		if (((code & 0x7E) >> 1 == 32) || ((code & 0x7E) >> 1 == 33) ||
			((code & 0x7E) >> 1 == 34) || ((code & 0x7E) >> 1 == 19) ||
			((code & 0x7E) >> 1 == 1) || ((code & 0x7E) >> 1 == 20) ||
			((code & 0x7E) >> 1 == 39))
		{
			is_find_start = true;
			i += start_code;
			break;		
		}
	}

	for (; i < bytes_read - 5; i++) 
	{
		if(m_buf[i] == 0 && m_buf[i + 1] == 0 && m_buf[i + 2] == 1)
		{
			start_code = 3;
		}
		else if(m_buf[i] == 0 && m_buf[i + 1] == 0 && m_buf[i + 2] == 0 && m_buf[i + 3] == 1) 
		{
			start_code = 4;
		}
		else   
		{
			continue;
		}
		
		code = m_buf[i + start_code];
		if(((code & 0x7E) >> 1 == 32) || ((code & 0x7E) >> 1 == 33) ||
		((code & 0x7E) >> 1 == 34) || ((code & 0x7E) >> 1 == 19) ||
		((code & 0x7E) >> 1 == 1) || ((code & 0x7E) >> 1 == 20) ||
		((code & 0x7E) >> 1 == 39))
		{
			is_find_end = true;
			break;	
		}      
	}

	bool flag = false;
	if(is_find_start && !is_find_end && m_count > 0) 
	{        
		flag = is_find_end = true;
		i = bytes_read;
		*end = true;
	}

	if(!is_find_start || !is_find_end) 
	{
		this->Close();
		return -1;
	}
	int size = (i <= in_buf_size ? i : in_buf_size);
	memcpy(in_buf, m_buf, size); 

	if(!flag) 
	{
		m_count += 1;
		m_bytes_used += i;
	}
	else 
	{
		m_count = 0;
		m_bytes_used = 0;
	}

	fseek(m_file, m_bytes_used, SEEK_SET);
	return size;
}


int test_rtsp_h265(void)
{
	std::shared_ptr<xop::RtspServer> h265_server;
	xop::MediaSessionId h265_session_id;

	H265File h265_file;
	if (!h265_file.Open("test.h265"))
	{
		return 0;
	}

	std::string suffix = "codec2021";
	std::string ip = "127.0.0.1";
	std::string port = "554";
	std::string rtsp_url = "rtsp://" + ip + ":" + port + "/" + suffix;

	std::shared_ptr<xop::EventLoop> event_loop(new xop::EventLoop());
	h265_server = xop::RtspServer::Create(event_loop.get());

	if (!h265_server->Start("0.0.0.0", atoi(port.c_str()))) {
		printf("RTSP Server listen on %s failed.\n", port.c_str());
		return 0;
	}

#ifdef AUTH_CONFIG
	server->SetAuthConfig("-_-", "admin", "12345");
#endif

	xop::MediaSession* session = xop::MediaSession::CreateNew("codec2021");
	session->AddSource(xop::channel_0, xop::H265Source::CreateNew());
	//session->StartMulticast(); 
	session->SetNotifyCallback([](xop::MediaSessionId session_id, uint32_t clients) {
		std::cout << "The number of rtsp clients: " << clients << std::endl;
		});

	h265_session_id = h265_server->AddSession(session);

	std::cout << "Play URL: " << rtsp_url << std::endl;

	std::thread t1(SendFrameThread, h265_server.get(), h265_session_id, &h265_file);

	while (1)
	{
		xop::Timer::Sleep(100);
	}

	return 0;
}


int main(int argc, char** argv)
{
	if(argc != 2) 
	{
		printf("Usage: %s test.h265\n", argv[0]);
		return 0;
	}


	test_rtsp_h265();

	return 0;
}
