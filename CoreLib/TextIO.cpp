#include "TextIO.h"
#ifdef _WIN32
#include <Windows.h>
#define CONVERT_END_OF_LINE
#endif

namespace CoreLib
{
	namespace IO
	{
		using namespace CoreLib::Basic;

		class Utf8Encoding : public Encoding 
		{
		public:
			virtual void GetBytes(List<char> & result, const String & str) override
			{
				result.AddRange(str.Buffer(), str.Length());
			}
			virtual String ToString(const char * bytes, int /*length*/) override
			{
				return String(bytes);
			}
		};

		class Utf32Encoding : public Encoding
		{
		public:
			virtual void GetBytes(List<char> & result, const String & str) override
			{
				int ptr = 0;
				while (ptr < str.Length())
				{
					int codePoint = GetUnicodePointFromUTF8([&](int)
					{
						if (ptr < str.Length())
							return str[ptr++];
						else
							return '\0';
					});
					result.AddRange((char*)&codePoint, 4);
				}
			}
			virtual String ToString(const char * bytes, int length) override
			{
				StringBuilder sb;
				int * content = (int*)bytes;
				for (int i = 0; i < (length >> 2); i++)
				{
					char buf[5];
					int count = EncodeUnicodePointToUTF8(buf, content[i]);
					for (int j = 0; j < count; j++)
						sb.Append(buf[j]);
				}
				return sb.ProduceString();
			}
		};

		class Utf16Encoding : public Encoding //UTF16
		{
		private:
			bool reverseOrder = false;
		public:
			Utf16Encoding(bool pReverseOrder)
				: reverseOrder(pReverseOrder)
			{}
			virtual void GetBytes(List<char> & result, const String & str) override
			{
				int ptr = 0;
				while (ptr < str.Length())
				{
					int codePoint = GetUnicodePointFromUTF8([&](int)
					{
						if (ptr < str.Length())
							return str[ptr++];
						else
							return '\0';
					});
					unsigned short buffer[2];
					int count;
					if (!reverseOrder)
						count = EncodeUnicodePointToUTF16(buffer, codePoint);
					else
						count = EncodeUnicodePointToUTF16Reversed(buffer, codePoint);
					result.AddRange((char*)buffer, count * 2);
				}
			}
			virtual String ToString(const char * bytes, int length) override
			{
				int ptr = 0;
				StringBuilder sb;
				while (ptr < length)
				{
					int codePoint = GetUnicodePointFromUTF16([&](int)
					{
						if (ptr < length)
							return bytes[ptr++];
						else
							return '\0';
					});
					char buf[5];
					int count = EncodeUnicodePointToUTF8(buf, codePoint);
					for (int i = 0; i < count; i++)
						sb.Append(buf[i]);
				}
				return sb.ProduceString();
			}
		};

		Utf8Encoding __utf8Encoding;
		Utf16Encoding __utf16Encoding(false);
		Utf16Encoding __utf16EncodingReversed(true);
		Utf32Encoding __utf32Encoding;

		Encoding * Encoding::UTF8 = &__utf8Encoding;
		Encoding * Encoding::UTF16 = &__utf16Encoding;
		Encoding * Encoding::UTF16Reversed = &__utf16EncodingReversed;
		Encoding * Encoding::UTF32 = &__utf32Encoding;

		const unsigned short Utf16Header = 0xFEFF;
		const unsigned short Utf16ReversedHeader = 0xFFFE;

		StreamWriter::StreamWriter(const String & path, Encoding * encoding)
		{
			this->stream = new FileStream(path, FileMode::Create);
			this->encoding = encoding;
			if (encoding == Encoding::UTF16)
			{
				this->stream->Write(&Utf16Header, 2);
			}
			else if (encoding == Encoding::UTF16Reversed)
			{
				this->stream->Write(&Utf16ReversedHeader, 2);
			}
		}
		StreamWriter::StreamWriter(RefPtr<Stream> stream, Encoding * encoding)
		{
			this->stream = stream;
			this->encoding = encoding;
			if (encoding == Encoding::UTF16)
			{
				this->stream->Write(&Utf16Header, 2);
			}
			else if (encoding == Encoding::UTF16Reversed)
			{
				this->stream->Write(&Utf16ReversedHeader, 2);
			}
		}
		void StreamWriter::Write(const String & str)
		{
			encodingBuffer.Clear();
			StringBuilder sb;
			String newLine;
#ifdef _WIN32
			newLine = "\r\n";
#else
			newLine = "\n";
#endif
			for (int i = 0; i < str.Length(); i++)
			{
				if (str[i] == '\r')
					sb << newLine;
				else if (str[i] == '\n')
				{
					if (i == 0 || str[i - 1] != '\r')
						sb << newLine;
				}
				else
					sb << str[i];
			}
			encoding->GetBytes(encodingBuffer, sb.ProduceString());
			stream->Write(encodingBuffer.Buffer(), encodingBuffer.Count());
		}
		void StreamWriter::Write(const char * str)
		{
			Write(String(str));
		}

		StreamReader::StreamReader(const String & path)
		{
			stream = new FileStream(path, FileMode::Open);
			ReadBuffer();
			encoding = DetermineEncoding();
			if (encoding == 0)
				encoding = Encoding::UTF8;
		}
		StreamReader::StreamReader(RefPtr<Stream> stream, Encoding * encoding)
		{
			this->stream = stream;
			this->encoding = encoding;
			ReadBuffer();
			auto determinedEncoding = DetermineEncoding();
			if (this->encoding == nullptr)
				this->encoding = determinedEncoding;
		}

		Encoding * StreamReader::DetermineEncoding()
		{
			if (buffer.Count() >= 3 && (unsigned char)(buffer[0]) == 0xEF && (unsigned char)(buffer[1]) == 0xBB && (unsigned char)(buffer[2]) == 0xBF)
			{
				ptr += 3;
				return Encoding::UTF8;
			}
			else if (*((unsigned short*)(buffer.Buffer())) == 0xFEFF)
			{
				ptr += 2;
				return Encoding::UTF16;
			}
			else if (*((unsigned short*)(buffer.Buffer())) == 0xFFFE)
			{
				ptr += 2;
				return Encoding::UTF16Reversed;
			}
			else
			{
#ifdef _WIN32
				int flag = IS_TEXT_UNICODE_SIGNATURE | IS_TEXT_UNICODE_REVERSE_SIGNATURE | IS_TEXT_UNICODE_STATISTICS | IS_TEXT_UNICODE_ASCII16;
				int rs = IsTextUnicode(buffer.Buffer(), buffer.Count(), &flag);
				if (rs)
				{
					if (flag & (IS_TEXT_UNICODE_SIGNATURE | IS_TEXT_UNICODE_STATISTICS))
						return Encoding::UTF16;
					else if (flag & (IS_TEXT_UNICODE_SIGNATURE | IS_TEXT_UNICODE_STATISTICS))
						return Encoding::UTF16Reversed;
					else if (flag & IS_TEXT_UNICODE_ASCII16)
						return Encoding::UTF8;
				}
#endif 
				return Encoding::UTF8;
			}
		}
		
		void StreamReader::ReadBuffer()
		{
			buffer.SetSize(4096);
			auto len = stream->Read(buffer.Buffer(), buffer.Count());
			buffer.SetSize((int)len);
			ptr = 0;
		}

		char StreamReader::ReadBufferChar()
		{
			if (ptr<buffer.Count())
			{
				return buffer[ptr++];
			}
			if (!stream->IsEnd())
				ReadBuffer();
			if (ptr<buffer.Count())
			{
				return buffer[ptr++];
			}
			return 0;
		}
		int TextReader::Read(char * destBuffer, int length)
		{
			int i = 0;
			for (i = 0; i<length; i++)
			{
				try
				{
					auto ch = Read();
					if (IsEnd())
						break;
					if (ch == '\r')
					{
						if (Peak() == '\n')
							Read();
						break;
					}
					else if (ch == '\n')
					{
						break;
					}
					destBuffer[i] = ch;
				}
				catch (EndOfStreamException)
				{
					break;
				}
			}
			return i;
		}
		String StreamReader::ReadLine()
		{
			StringBuilder sb(256);
			while (!IsEnd())
			{
				try
				{
					auto ch = Read();
					if (IsEnd())
						break;
					if (ch == '\r')
					{
						if (Peak() == '\n')
							Read();
						break;
					}
					else if (ch == '\n')
					{
						break;
					}
					sb.Append(ch);
				}
				catch (EndOfStreamException)
				{
					break;
				}
			}
			return sb.ProduceString();
		}
		String StreamReader::ReadToEnd()
		{
			StringBuilder sb(16384);
			while (!IsEnd())
			{
				try
				{
					auto ch = Read();
					if (IsEnd())
						break;
					if (ch == '\r')
					{
						sb.Append('\n');
						if (Peak() == '\n')
							Read();
					}
					else
						sb.Append(ch);
				}
				catch (EndOfStreamException)
				{
					break;
				}
			}
			return sb.ProduceString();
		}
	}
}