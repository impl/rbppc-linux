/*
 * Copyright (C) 2008-2011 Noah Fontes <nfontes@invectorate.com>
 * Copyright (C) Mikrotik 2007
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include <linux/kernel.h>
#include <linux/pci.h>

static void fixup_pci(struct pci_dev *dev)
{
	if ((dev->class >> 8) == PCI_CLASS_BRIDGE_PCI) {
		/*
		 * Let the kernel itself set right memory windows.
		 */
		pci_write_config_word(dev, PCI_MEMORY_BASE, 0);
		pci_write_config_word(dev, PCI_MEMORY_LIMIT, 0);
		pci_write_config_word(dev, PCI_PREF_MEMORY_BASE, 0);
		pci_write_config_word(dev, PCI_PREF_MEMORY_LIMIT, 0);
		pci_write_config_byte(dev, PCI_IO_BASE, 0);
		pci_write_config_byte(dev, PCI_IO_LIMIT, 4 << 4);

		pci_write_config_byte(dev, PCI_COMMAND, PCI_COMMAND_MASTER |
				      PCI_COMMAND_MEMORY | PCI_COMMAND_IO);
		pci_write_config_byte(dev, PCI_CACHE_LINE_SIZE, 8);
	} else if (dev->vendor == 0x1957 &&
		   (dev->device == 0x32 || dev->device == 0x33)) {
		u16 val;
		pci_read_config_word(dev, 0x44, &val);
		pci_write_config_word(dev, 0x44, val | (1 << 10));
		pci_write_config_word(dev, PCI_LATENCY_TIMER, 0x00);
	} else
		pci_write_config_byte(dev, PCI_LATENCY_TIMER, 0x40);
}
DECLARE_PCI_FIXUP_HEADER(PCI_ANY_ID, PCI_ANY_ID, fixup_pci)

static void fixup_secondary_bridge(struct pci_dev *dev)
{
	pci_write_config_byte(dev, PCI_COMMAND, PCI_COMMAND_MASTER |
			      PCI_COMMAND_MEMORY | PCI_COMMAND_IO);

	/*
	 * Disable prefetched memory range.
	 */
	pci_write_config_word(dev, PCI_PREF_MEMORY_LIMIT, 0);
	pci_write_config_word(dev, PCI_PREF_MEMORY_BASE, 0x10);

	pci_write_config_word(dev, PCI_BASE_ADDRESS_0, 0);
	pci_write_config_word(dev, PCI_BASE_ADDRESS_1, 0);

	pci_write_config_byte(dev, PCI_CACHE_LINE_SIZE, 8);

	pci_write_config_byte(dev, 0xc0, 0x01);
}
DECLARE_PCI_FIXUP_HEADER(0x3388, 0x0021, fixup_secondary_bridge)
