#include <vector>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <errno.h>
#include <dirent.h>
#include <stdint.h>
#include <string_view>
#include <jni.h>

constexpr bool verbose=false;
enum
{
    TAG_MAKE=271,
    TAG_MODEL=272,
    TAG_CFA_PATTERN=33422,
    TAG_UNIQUE_CAMERA_MODEL=50708,
    TAG_BLACK_LEVEL_REPEAT_DIM=50713,
    TAG_BLACK_LEVEL=50714,
};

std::string uniqueCamModel;
std::vector<char> cfaPattern;
uint32_t cfaPatternOffset;
std::vector<uint16_t> blackLevelRepeatDim;
std::vector<uint32_t> blackLevel;
uint32_t blackLevelOffset;

void parseIFD(std::ifstream& file)
{
    std::ofstream log;//("/storage/emulated/0/dng-fixup.log", std::ios_base::app);
    enum Type { BYTE=1,ASCII,WORD,DWORD,RATIONAL,SBYTE,UNDEFINED,SSHORT,SLONG,SRATIONAL,FLOAT,DOUBLE };

    uint16_t entryCount;
    file.read(reinterpret_cast<char*>(&entryCount), sizeof entryCount);
    for(int i=0; i<entryCount; ++i)
    {
        uint16_t tag;
        file.read(reinterpret_cast<char*>(&tag), sizeof tag);
        uint16_t type;
        file.read(reinterpret_cast<char*>(&type), sizeof type);
        uint32_t len;
        file.read(reinterpret_cast<char*>(&len), sizeof len);
        uint32_t dataOffset;
        file.read(reinterpret_cast<char*>(&dataOffset), sizeof dataOffset);
        uint32_t unitSize;
        switch(type)
        {
        case BYTE  :
        case SBYTE :
        case ASCII : unitSize=1; break;
        case SSHORT:
        case WORD  : unitSize=2; break;
        case FLOAT:
        case SLONG :
        case DWORD : unitSize=4; break;
        case DOUBLE:
        case SRATIONAL:
        case RATIONAL : unitSize=8; break;
        default: unitSize=0; break;
        }
        if(len*unitSize<=4)
            dataOffset=file.tellg()-std::fstream::pos_type(sizeof dataOffset);

        if(verbose)
        {
            log << std::left
                << "Tag " << std::setw(5) << tag
                << ", type " << std::setw(2) << type
                << ", length " << std::setw(5) << len
                << ", offset " << std::setw(6) << dataOffset;
        }

        switch(type)
        {
        case ASCII:
        {
            const auto pos=file.tellg();
            file.seekg(dataOffset);
            std::string value(len, '\0');
            file.read(value.data(), value.size());
            file.seekg(pos);
            if(!value.empty() && value.back()!=0)
            {
                log << "Bad ASCII string: not NUL-terminated\n";
                std::exit(1);
            }
            if(!value.empty()) value.pop_back();

            if(verbose)
                log << ": \"" << value << '"';

            if(tag==TAG_UNIQUE_CAMERA_MODEL)
                uniqueCamModel=std::move(value);
            break;
        }
        case BYTE:
        {
            const auto pos=file.tellg();
            file.seekg(dataOffset);
            std::vector<char> value(len);
            file.read(value.data(), value.size());
            file.seekg(pos);

            if(verbose)
            {
                log << ": ";
                for(std::size_t i=0; i<value.size(); ++i)
                {
                    if(i>0) log << ',';
                    log << +uint8_t(value[i]);
                }
            }

            if(tag==TAG_CFA_PATTERN)
            {
                cfaPatternOffset=dataOffset;
                cfaPattern=std::move(value);
            }
            break;
        }
        case WORD:
        {
            const auto pos=file.tellg();
            file.seekg(dataOffset);
            std::vector<uint16_t> value(len);
            file.read(reinterpret_cast<char*>(value.data()), len*unitSize);
            file.seekg(pos);

            if(verbose)
            {
                log << ": ";
                for(std::size_t i=0; i<value.size(); ++i)
                {
                    if(i>0) log << ',';
                    log << value[i];
                }
            }

            if(tag==TAG_BLACK_LEVEL_REPEAT_DIM)
                blackLevelRepeatDim=value;
            break;
        }
        case RATIONAL:
        {
            const auto pos=file.tellg();
            file.seekg(dataOffset);
            std::vector<uint32_t> value(2*len);
            file.read(reinterpret_cast<char*>(value.data()), len*unitSize);
            file.seekg(pos);

            if(verbose)
            {
                log << ": {";
                for(std::size_t i=0; i<len; ++i)
                {
                    if(i>0) log << ',';
                    log << value[2*i] << "/" << value[2*i+1];
                }
                log << "} = {";
                for(std::size_t i=0; i<len; ++i)
                {
                    if(i>0) log << ',';
                    log << double(value[2*i])/value[2*i+1];
                }
                log << "}";
            }

            if(tag==TAG_BLACK_LEVEL)
            {
                blackLevel=std::move(value);
                blackLevelOffset=dataOffset;
            }
            break;
        }
        }
        if(verbose)
            log << "\n";
    }
}

int mainy()
{
    std::ofstream log;//("/storage/emulated/0/dng-fixup.log", std::ios_base::app);
    std::vector<std::string> filenames;
    const auto dirPath = std::string("/storage/emulated/0/DCIM/OpenCamera/");
    if(const auto dp = opendir(dirPath.c_str()))
    {
        dirent* ep;
        while((ep = readdir(dp))!=nullptr)
            filenames.emplace_back(dirPath+ep->d_name);
        closedir(dp);
    }
    else
    {
        const auto err=errno;
        log << "Failed to open directory: " << std::strerror(err) << "\n";
        return 1;
    }
    for(const auto filename : filenames)
    {
        if(filename.size() <= 4) continue;
        const auto ext=filename.substr(filename.size()-4);
        if(ext!=".dng" && ext!=".DNG") continue;

        std::ifstream in(filename);
        if(!in)
        {
            log << "Failed to open for reading file \"" << filename << "\"\n";
            return 1;
        }
        in.exceptions(std::ifstream::badbit|std::ifstream::failbit);
        log << "Trying file \"" << filename << "\"...\n";
        try
        {
            char dword[4];
            in.read(dword, sizeof dword);
            if(dword[0]!='I' || dword[1]!='I' || dword[2]!=42 || dword[3]!=0)
            {
                log << "This program only supports little-endian TIFF files\n";
                continue;
            }
            uint32_t ifd;
            in.read(reinterpret_cast<char*>(&ifd), sizeof ifd);
            if(ifd&1)
            {
                log << "First image directory must be 2-byte-aligned, but its offset is " << ifd << "\n";
                continue;
            }
            for(int n=0; ifd; ++n)
            {
                in.seekg(ifd);
                if(verbose)
                    log << "Image File Directory " << n << "\n";
                parseIFD(in);
                in.read(reinterpret_cast<char*>(&ifd), sizeof ifd);
            }
        }
        catch(std::exception&)
        {
            const auto err=errno;
            log << "Error reading file \"" << filename << "\": last error: " << std::strerror(err) << "\n";
            continue;
        }

        if(uniqueCamModel!="SM-A320F-samsung-samsung")
        {
            if(verbose)
                log << "Not a relevant camera model\n";
            continue;
        }

        if(blackLevelRepeatDim!=std::vector<uint16_t>{2,2})
        {
            if(verbose)
                log << "Unexpected black level repeat dimension\n";
            continue;
        }

        const std::vector<uint32_t> expectedWrongBlackLevel{0,1, 0,1, 0,1, 0,1};
        const std::vector<uint32_t> correctBlackLevel{331,5, 331,5, 331,5, 331,5};

        bool needsCFAcorrection=false, needsBlackLevelCorrection=false;
        if(blackLevel==expectedWrongBlackLevel)
        {
            if(verbose)
                log << "Black level needs fixup\n";
            needsBlackLevelCorrection=true;
        }
        else if(blackLevel==correctBlackLevel)
        {
            if(verbose)
                log << "Black level is already OK.\n";
        }
        else
        {
            if(verbose)
                log << "Unexpected black level. Won't touch the file.\n";
            continue;
        }


        const std::vector<char> correctCFApattern{0,1,1,2};
        const std::vector<char> expectedWrongCFApattern{1,0,2,1};
        if(cfaPattern==expectedWrongCFApattern)
        {
            if(verbose)
                log << "CFA pattern field needs to be fixed up\n";
            needsCFAcorrection=true;
        }
        else if(cfaPattern==correctCFApattern)
        {
            if(verbose)
                log << "CFA pattern is already OK.\n";
        }
        else
        {
            log << "Unexpected CFA pattern. Won't touch the file.\n";
            continue;
        }

        if(!needsCFAcorrection && !needsBlackLevelCorrection)
            continue;

        in.close();
        std::ofstream out(filename, std::ios_base::in|std::ios_base::out); // "r+" fopen mode
        if(!out)
        {
            log << "Failed to open file for writing\n";
            continue;
        }
        out.exceptions(std::ifstream::badbit|std::ifstream::failbit);
        try
        {
            if(needsCFAcorrection)
            {
                if(verbose)
                    log << "Writing CFA pattern to offset " << cfaPatternOffset << "\n";
                out.seekp(cfaPatternOffset);
                out.write(correctCFApattern.data(), correctCFApattern.size());
            }

            if(needsBlackLevelCorrection)
            {
                if(verbose)
                    log << "Writing black level to offset " << blackLevelOffset << "\n";
                out.seekp(blackLevelOffset);
                out.write(reinterpret_cast<const char*>(correctBlackLevel.data()),
                          correctBlackLevel.size()*sizeof correctBlackLevel[0]);
            }
        }
        catch(std::exception&)
        {
            log << "Failed to alter the file\n";
            continue;
        }
    }
    return 0;
}

extern "C" JNIEXPORT void JNICALL Java_net_sourceforge_opencamera_cameracontroller_RawImage_dngFixup(JNIEnv*, jobject)
{
    mainy();
}
