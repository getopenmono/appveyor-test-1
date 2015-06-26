#include "ElfProgrammer.h"
#include "ElfReader.h"
#include "IProgrammer.h"
#include "IDeviceCommunicator.h"
#include "CombinedMemory.h"
#include "OutputCollector.h"
#include "cybtldr_api.h"
#include "cybtldr_command.h"
#include "elfio.hpp"
#include <cmath>

namespace {

size_t max (size_t x, size_t y)
{
	if (x > y) return x;
	else return y;
}

size_t divideAndCeil (size_t dividend, size_t divisor)
{
	double quotient = (double)dividend / (double)divisor;
	return ceil(quotient);
}

struct ConfiguredFlashRow
{
	static const size_t BUFSIZE = 0x120;
	char buffer[BUFSIZE];
	size_t arrayId;
	size_t rowNum;
	ConfiguredFlashRow (size_t index, IMemorySection & program, IMemorySection & config)
	{
		// Memory must be alligned on Flash line.
		size_t address = program.address();
		if ((address & 0xFF) != 0x00) throw "Misaligned program";
		// Calculate the right part of the program according to index.
		address += index * 0x100;
		for (size_t i = 0; i < 0x100; ++i) buffer[i] = program[address+i];
		size_t highAddress = address >> 8;
		arrayId = highAddress >> 8;
		rowNum = highAddress & 0xFF;
		size_t configAddress = config.address() + index * 0x20;
		for (size_t i = 0; i < 0x20; ++i) buffer[i+0x100] = config[configAddress+i];
	}
	ConfiguredFlashRow (size_t address, IMemorySection & metadata)
	{
		// Memory must be alligned on Flash line.
		if ((address & 0xFF) != 0x00) throw "Misaligned metadata";
		for (size_t i = 0; i < 0x100; ++i) buffer[i] = metadata[address+i];
		size_t highAddress = address >> 8;
		arrayId = highAddress >> 8;
		rowNum = highAddress & 0xFF;
		for (size_t i = 0; i < 0x20; ++i) buffer[i+0x100] = 0;
	}
	int writeToDevice () const
	{
		int err = CyBtldr_ProgramRow(arrayId,rowNum,(char unsigned *)&buffer,BUFSIZE);
		if (CYRET_SUCCESS != err) return err;
		char unsigned checksum = CyBtldr_ComputeChecksum((char unsigned *)&buffer,BUFSIZE);
		err = CyBtldr_VerifyRow(arrayId,rowNum,checksum);
		return err;
	}
};

} // namespace

#define OUTPUT(level) OUTPUTCOLLECTOR_LINE((*output),level)

ElfProgrammer::ElfProgrammer (QFileInfo & file, IDeviceCommunicator * device)
: ProgrammerBase(file,device)
, bootloaderVersion(0)
, bootloaderFound(false)
{
}

ElfProgrammer::~ElfProgrammer ()
{
	if (bootloaderFound) CyBtldr_SafeEndBootloadOperation();
}

ProgramStatus ElfProgrammer::program ()
{
	std::string const stringPath = file.filePath().toStdString();
	char const * path = stringPath.c_str();
	OUTPUT(5) << "ElfProgrammer " << path;
	if (! setUpElfReader()) return ProgrammerCorruptProgram;
	setupProgramMemory();
	setupConfigMemory();
	setupMetadataMemory();
	setupSiliconId();
	if (!code || !config || !metadata) return ProgrammerCorruptProgram;
	std::ios::fmtflags flags(output->getFlags());
	OUTPUT(2) << "Program: " << std::hex << code->address() << "-" << code->address()+code->size();
	OUTPUT(2) << "Config: " << std::hex << config->address() << "-" << config->address()+config->size();
	OUTPUT(2) << "Meta: " << std::hex << metadata->address() << "-" << metadata->address()+metadata->size();
	output->setFlags(flags);
	useInvertedSummationOfAllBytesChecksum();
	if (! startBootloader()) return ProgrammerNoConnectionToMonoDevice;
	if (! transferProgramToBooloader()) return ProgrammerFailed;
	return ProgrammerSuccess;
}

bool ElfProgrammer::transferProgramToBooloader ()
{
	CyBtldr_ProgressUpdate * updater = getCybtldrProgressUpdate();
	// Memory must be alligned on Flash line.
	size_t address = code->address();
	if ((address & 0xFF) != 0x00) throw "Misaligned program";
	size_t rows = max(divideAndCeil(code->size(),0x100),divideAndCeil(config->size(),0x20));
	for (size_t i = 0; i < rows; ++i)
	{
		ConfiguredFlashRow row(i,*code,*config);
		OUTPUT(5) << "arrayId=" << row.arrayId;
		OUTPUT(5) << "rowNum=" << row.rowNum;
		output->outputHex(5,row.buffer,row.BUFSIZE,32);
		int res = row.writeToDevice();
		if (CYRET_SUCCESS != res)
		{
			output->error() << "Programming row " << i << " failed: " << res;
			return false;
		}
		updater(row.arrayId,row.rowNum);
	}
	// Align row start for Metadata.
	size_t metaRowStartAddress = metadata->address() & 0xFFFFFF00;
	size_t metaRowSize = metadata->address()+metadata->size()-metaRowStartAddress;
	// The empirical data we have only has short meta data sections, so we do
	// not know what the layout would be if the meta data section was longer
	// than one flash row.  Maybe the last 0x20 bytes are just zero padded?
	if (metaRowSize > 0x100) return ProgrammerUnsupportedMetaData;
	ConfiguredFlashRow row(metaRowStartAddress,*metadata);
	OUTPUT(5) << "arrayId=" << row.arrayId;
	OUTPUT(5) << "rowNum=" << row.rowNum;
	output->outputHex(5,row.buffer,row.BUFSIZE,32);
	if (CYRET_SUCCESS != row.writeToDevice()) return false;
	updater(row.arrayId,row.rowNum);
	const unsigned long BL_VER_SUPPORT_VERIFY = 0x010214;
	if (bootloaderVersion >= BL_VER_SUPPORT_VERIFY)
	{
		if (CYRET_SUCCESS != CyBtldr_VerifyApplication()) return false;
	}
	return true;
}

void ElfProgrammer::useInvertedSummationOfAllBytesChecksum ()
{
	CyBtldr_SetCheckSumType(SUM_CHECKSUM);
}

bool ElfProgrammer::startBootloader ()
{
	//const long unsigned siliconId = 0x2e123069;
	const char unsigned siliconRev = 0;
	cyComms = getCybtldrCommPack();
	int result = CyBtldr_StartBootloadOperation(&cyComms,siliconId,siliconRev,&bootloaderVersion);
	if (CYRET_SUCCESS == result)
	{
		bootloaderFound = true;
		return true;
	}
	else
	{
		OUTPUT(3) << "Could not start bootloader, CYRET=" << result;
		return false;
	}
}

bool ElfProgrammer::setUpElfReader ()
{
	ELFIO::elfio * elfio = new ELFIO::elfio();
	if (! elfio->load(file.filePath().toStdString()))
	{
		output->error() << "Could not load ELF program " << file.filePath().toStdString();
		return false;
	}
	reader = std::unique_ptr<ElfReader>(new ElfReader(elfio));
	return true;
}

void ElfProgrammer::setupProgramMemory ()
{
	std::unique_ptr<IMemorySection> text(reader->getSection(".text"));
	if (! text)
	{
		output->error() << "No section .text in ELF program " << file.filePath().toStdString();
		return;
	}
	std::unique_ptr<IMemorySection> rodata(reader->getSection(".rodata"));
	if (! rodata)
	{
		output->error() << "No section .rodata in ELF program " << file.filePath().toStdString();
		return;
	}
	code = std::unique_ptr<IMemorySection>
	(
		new CombinedMemory(std::move(text),std::move(rodata))
	);
}

void ElfProgrammer::setupConfigMemory ()
{
	config = std::unique_ptr<IMemorySection>(reader->getSection(".cyconfigecc"));
	if (! config)
	{
		output->error() << "No section .cyconfigecc in ELF program " << file.filePath().toStdString();
	}
}

void ElfProgrammer::setupMetadataMemory ()
{
	metadata = std::unique_ptr<IMemorySection>(reader->getSection(".cyloadablemeta"));
	if (! metadata)
	{
		output->error() << "No section .cyloadablemeta in ELF program " << file.filePath().toStdString();
	}
}

void ElfProgrammer::setupSiliconId ()
{
	std::unique_ptr<IMemorySection> cymeta(reader->getSection(".cymeta"));
	if (! cymeta)
	{
		output->error() << "No section .cymeta in ELF program " << file.filePath().toStdString();
	}
	size_t startAddress = cymeta->address() + 2;
	siliconId =
		((*cymeta)[startAddress+0]<<24) +
		((*cymeta)[startAddress+1]<<16) +
		((*cymeta)[startAddress+2]<<8) +
		(*cymeta)[startAddress+3];
	std::ios::fmtflags flags(output->getFlags());
	OUTPUT(2) << "SiliconId " << std::hex << siliconId;
	output->setFlags(flags);
}
