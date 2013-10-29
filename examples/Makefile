LDFLAGS= -L../rump/lib -Wl,-R../rump/lib -Wl,--no-as-needed -lrumpnet_netmapif -lrumpnet_netinet -lrumpnet_net -lrumpnet -lrumpnet_config -lrump -lrumpuser

CFLAGS=-I../rump/include -Wall

all: netmapcat netmapsend netmapreceive

clean:
	rm -f netmapcat netmapsend netmapreceive
