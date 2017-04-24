# Default configuration for ppc64-softmmu

include pci.mak
include sound.mak
include usb.mak
CONFIG_VIRTIO_VGA=y
CONFIG_ESCC=y
CONFIG_M48T59=y
CONFIG_IPMI=y
CONFIG_IPMI_LOCAL=y
CONFIG_IPMI_EXTERN=y
CONFIG_ISA_IPMI_BT=y
CONFIG_SERIAL=y
CONFIG_PARALLEL=y
CONFIG_I8254=y
CONFIG_PCKBD=y
CONFIG_FDC=y
CONFIG_I8257=y
CONFIG_I82374=y
CONFIG_OPENPIC=y
CONFIG_PREP_PCI=y
CONFIG_I82378=y
CONFIG_PC87312=y
CONFIG_MACIO=y
CONFIG_PCSPK=y
CONFIG_CUDA=y
CONFIG_ADB=y
CONFIG_MAC_NVRAM=y
CONFIG_MAC_DBDMA=y
CONFIG_HEATHROW_PIC=y
CONFIG_GRACKLE_PCI=y
CONFIG_UNIN_PCI=y
CONFIG_DEC_PCI=y
CONFIG_PPCE500_PCI=y
CONFIG_IDE_ISA=y
CONFIG_IDE_CMD646=y
CONFIG_IDE_MACIO=y
CONFIG_NE2000_ISA=y
CONFIG_PFLASH_CFI01=y
CONFIG_PFLASH_CFI02=y
CONFIG_PTIMER=y
CONFIG_I8259=y
CONFIG_XILINX=y
CONFIG_XILINX_ETHLITE=y
CONFIG_PSERIES=y
CONFIG_POWERNV=y
CONFIG_PREP=y
CONFIG_MAC=y
CONFIG_E500=y
CONFIG_OPENPIC_KVM=$(and $(CONFIG_E500),$(CONFIG_KVM))
CONFIG_PLATFORM_BUS=y
CONFIG_ETSEC=y
CONFIG_LIBDECNUMBER=y
# For pSeries
CONFIG_XICS=$(CONFIG_PSERIES)
CONFIG_XICS_SPAPR=$(CONFIG_PSERIES)
CONFIG_XICS_KVM=$(and $(CONFIG_PSERIES),$(CONFIG_KVM))
# For PReP
CONFIG_SERIAL_ISA=y
CONFIG_MC146818RTC=y
CONFIG_ISA_TESTDEV=y
CONFIG_MEM_HOTPLUG=y
CONFIG_RS6000_MC=y
