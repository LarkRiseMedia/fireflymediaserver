Event.observe(window,'load',function (e) {Config.init();});

// Config isn't defined until after the Event.observe above
// I could have put it below Config = ... but I want all window.load events
// at the start of the file
function init() {
  Config.init();
}

var Config = {
  layout: '',
  configPath: '',
  configOptionValues: '',
  init: function () {
    new Ajax.Request('/config.xml',{method: 'get',onComplete: Config.storeConfigLayout});
  },
  storeConfigLayout: function (request) {
    Config.layout = request.responseXML;
    new Ajax.Request('/xml-rpc?method=stats',{method: 'get',onComplete: Config.updateStatus});
  },
  updateStatus: function (request) {
    Config.configPath = Element.textContent(request.responseXML.getElementsByTagName('config_path')[0]);
//    $('config_path').appendChild(document.createTextNode(
      
//    );
    new Ajax.Request('/xml-rpc?method=config',{method: 'get',onComplete: Config.showConfig});  
  },
  showConfig: function (request) {
    Config.configOptionValues = request.responseXML;
    var sections = $A(Config.layout.getElementsByTagName('section'));
    sections.each(function (section) {
      var head = document.createElement('div');
      head.className= 'naviheader';
      head.appendChild(document.createTextNode(section.getAttribute('name')));
      var body = document.createElement('div');
      body.className = 'navibox';
      if ('Server' == section.getAttribute('name')) {
        body.appendChild(Builder.node('span',{id:'config_path'},'Config File'));
        body.appendChild(document.createTextNode(Config.configPath));
        body.appendChild(Builder.node('br'));
        body.appendChild(Builder.node('div',{style: 'clear: both;'}));
      }
      $A(section.getElementsByTagName('item')).each(function (item) {
        body.appendChild(Config._buildItem(item));
      });
      $('theform').appendChild(head);
      $('theform').appendChild(body);
    });  
  },
  _getConfigOptionValue: function(id,multiple) {
    if (multiple) {
      var ret = [];
      var option = Config.configOptionValues.getElementsByTagName(id);
      if (option.length > 0) { 
        $A(option[0].getElementsByTagName('item')).each(function (item) {
          ret.push(Element.textContent(item));
        });
      } else {
        ret.push('');
      }
      return ret;
    } else {
      var value = Config.configOptionValues.getElementsByTagName(id);
      if (value.length > 0) {
        return Element.textContent(value[0]);
      } else {
        return '';
      }
    }
  },
  _buildItem: function(item) {
    var ret;
    var itemId = item.getAttribute('id');
    var href;
    var span;
    switch(Element.textContent(item.getElementsByTagName('type')[0])) {
      case 'short_text':
        ret = BuildElement.input(itemId,
                                 Element.textContent(item.getElementsByTagName('name')[0]),
                                 Config._getConfigOptionValue(itemId),20,
                                 Element.textContent(item.getElementsByTagName('short_description')[0]),
                                 '');
        break;
      case 'long_text':
        ret = BuildElement.input(itemId,
                                 Element.textContent(item.getElementsByTagName('name')[0]),
                                 Config._getConfigOptionValue(itemId),80,
                                 Element.textContent(item.getElementsByTagName('short_description')[0]),                                     
                                 '');
        break;
      case 'short_text_multiple':
        ret = document.createDocumentFragment();
        Config._getConfigOptionValue(itemId,true).each(function (value,i) {
          var span = document.createElement('span');
          span.appendChild(BuildElement.input(itemId+i,
                                             Element.textContent(item.getElementsByTagName('name')[0]),
                                             value,20,
                                             Element.textContent(item.getElementsByTagName('short_description')[0])
          ));
          ret.appendChild(span);                                   
        });
        href = Builder.node('a',{href:'javascript://',className:'addItemHref'},
                                     Element.textContent(item.getElementsByTagName('add_item_text')[0]));
        ret.appendChild(href);
        Event.observe(href,'click',Config._addItem);
        ret.appendChild(Builder.node('div',{style:'clear: both'}));
        break;                                  
      case 'long_text_multiple':
//###TODO Do something smart instead of just copying
        ret = document.createDocumentFragment();
        Config._getConfigOptionValue(itemId,true).each(function (value,i) {
          var span = document.createElement('span');
          span.appendChild(BuildElement.input(itemId+i,
                                             Element.textContent(item.getElementsByTagName('name')[0]),
                                             value,80,
                                             Element.textContent(item.getElementsByTagName('short_description')[0])
          ));
          ret.appendChild(span);                                             
        });
        href = Builder.node('a',{href:'javascript://',className:'addItemHref'},
                                     Element.textContent(item.getElementsByTagName('add_item_text')[0]));
        ret.appendChild(href);
        Event.observe(href,'click',Config._addItem);
        ret.appendChild(Builder.node('div',{style:'clear: both'}));
        break;                                  
      case 'select':
        var value = Config._getConfigOptionValue(itemId);
        if (!value) {
          value = Element.textContent(item.getElementsByTagName('default_value')[0]);
        }
        ret = BuildElement.select(itemId,
                                  Element.textContent(item.getElementsByTagName('name')[0]),
                                  item.getElementsByTagName('option'),
                                  value,
                                  Element.textContent(item.getElementsByTagName('short_description')[0])
                                    );
        break;
    }
    return ret;
  },
  _addItem: function(e) {
    var span = Event.element(e);
    while (span.nodeName.toLowerCase() != 'span') {
      span = span.previousSibling;
    }
    var frag = document.createDocumentFragment();
    span = span.cloneNode(true);
    span.getElementsByTagName('label')[0].setAttribute('for','hej');
    span.getElementsByTagName('input')[0].id = 'hej';
    span.getElementsByTagName('input')[0].value = '';
    
    var src = Event.element(e);
    src.parentNode.insertBefore(span,src);
    
  }
}
var BuildElement = {
  input: function(id,displayName,value,size,short_description,long_description) {

    var frag = document.createDocumentFragment();
    var label = document.createElement('label');

    label.setAttribute('for',id);
    label.appendChild(document.createTextNode(displayName));
    frag.appendChild(label);

    frag.appendChild(Builder.node('input',{id:id,name:id,className: 'text',
                                           value: value,size: size}));
    frag.appendChild(document.createTextNode('\u00a0'));                                              
    frag.appendChild(document.createTextNode(short_description));
    frag.appendChild(Builder.node('br'));

    return frag;
  },
  select: function(id,displayName,options,value,short_description,long_description) {
    var frag = document.createDocumentFragment();
    var label = document.createElement('label');
    label.setAttribute('for',id);
    label.appendChild(document.createTextNode(displayName));
    frag.appendChild(label);
    
    var select = Builder.node('select',{id: id,name: id, size: 1});
    $A(options).each(function (option) {
      select.appendChild(Builder.node('option',{value: option.getAttribute('value')},
                                      Element.textContent(option)));
    });
    select.value = value;
    frag.appendChild(select);
    frag.appendChild(document.createTextNode('\u00a0'));                                              
    frag.appendChild(document.createTextNode(short_description));
    frag.appendChild(Builder.node('br'));

    return frag;
  }
    
}
Object.extend(Element, {
  removeChildren: function(element) {
  while(element.hasChildNodes()) {
    element.removeChild(element.firstChild);
  }
  },
  textContent: function(node) {
  if ((!node) || !node.hasChildNodes()) {
    // Empty text node
    return '';
  } else {
    if (node.textContent) {
    // W3C ?
    return node.textContent;
    } else if (node.text) {
    // IE
    return node.text;
    }
  }
  // We shouldn't end up here;
  return '';
  }
});
/* Detta script finns att hamta pa http://www.jojoxx.net och 
   far anvandas fritt sa lange som dessa rader star kvar. */ 
function DataDumper(obj,n,prefix){
	var str=""; prefix=(prefix)?prefix:""; n=(n)?n+1:1; var ind=""; for(var i=0;i<n;i++){ ind+="  "; }
	if(typeof(obj)=="string"){
		str+=ind+prefix+"String:\""+obj+"\"\n";
	} else if(typeof(obj)=="number"){
		str+=ind+prefix+"Number:"+obj+"\n";
	} else if(typeof(obj)=="function"){
		str+=ind+prefix+"Function:"+obj+"\n";
	} else if(typeof(obj) == 'boolean') {
       str+=ind+prefix+"Boolean:" + obj + "\n";
	} else {
		var type="Array";
		for(var i in obj){ type=(type=="Array"&&i==parseInt(i))?"Array":"Object"; }
		str+=ind+prefix+type+"[\n";
		if(type=="Array"){
			for(var i in obj){ str+=DataDumper(obj[i],n,i+"=>"); }
		} else {
			for(var i in obj){ str+=DataDumper(obj[i],n,i+"=>"); }
		}
		str+=ind+"]\n";
	}
	return str;
}
